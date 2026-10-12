using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net.WebSockets;
using System.Text.Json;
using Keysynth.Api.Auth;
using Keysynth.Core;
using Microsoft.Extensions.Options;

namespace Keysynth.Api.Relay;

/// <summary>
/// In-memory relay between a user's hosts (engines) and controllers (browser tabs), docs/PROTOCOL.md *Relay*.
/// One room per local user id; nothing ever crosses rooms. Room state changes and fan-out happen under the room lock;
/// fan-out only enqueues (never blocks). A background sweep sends pings, closes silent / too old sockets and
/// revalidates host tokens. Single API instance → in-memory is enough (scale-out would need a shared bus).
/// </summary>
public sealed class RelayHub(
    IOptions<RelayOptions> options,
    IServiceScopeFactory scopes,
    IClock clock,
    IHostApplicationLifetime lifetime,
    ILogger<RelayHub> log) : BackgroundService
{
    private sealed class Room
    {
        public readonly Dictionary<string, RelayConnection> Hosts = new(StringComparer.Ordinal);
        public readonly Dictionary<string, RelayConnection> Controllers = new(StringComparer.Ordinal);

        /// <summary>Hosts seen since the process started (deviceId → last hello), listed as offline when gone.</summary>
        public readonly Dictionary<string, (string Name, string Version)> Known = new(StringComparer.Ordinal);
    }

    private const int MaxNameLength = Limits.MaxDeviceNameLength;
    private const int MaxVersionLength = 32;

    private static readonly JsonDocumentOptions ParseOptions = new() { MaxDepth = 256 };

    private readonly RelayOptions _o = options.Value;
    private readonly ConcurrentDictionary<long, Room> _rooms = new();
    private readonly ConcurrentDictionary<RelayConnection, byte> _all = new();

    private volatile bool _stopping;

    public RelayOptions Options => _o;

    /// <summary>Sockets currently running (incl. ones waiting for their close handshake).</summary>
    public int ConnectionCount => _all.Count;

    /// <summary>Runs one accepted socket until it closes: join, read loop, leave.</summary>
    public async Task RunAsync(RelayConnection conn, WebSocket socket, CancellationToken aborted)
    {
        conn.Closing = Leave;
        var writer = conn.RunWriterAsync();
        _all[conn] = 0;
        if (_stopping)
            conn.Close(RelayClose.GoingAway, "shutdown");
        try
        {
            if (Join(conn))
                await ReadLoopAsync(conn, socket, aborted);
        }
        finally
        {
            Leave(conn);
            _all.TryRemove(conn, out _);
            conn.Close(RelayClose.Normal, "");
            await writer;
            conn.DisposeAbort();
            log.LogInformation("relay {Role} {LogId} user {UserId} closed ({Code})", Role(conn), conn.LogId, conn.UserId, conn.CloseCode);
        }
    }

    private static string Role(RelayConnection c) => c.Role == RelayRole.Host ? "host" : "controller";

    private async Task ReadLoopAsync(RelayConnection conn, WebSocket socket, CancellationToken aborted)
    {
        var cap = conn.Role == RelayRole.Host ? _o.HostFrameBytes : _o.ControllerFrameBytes;
        var message = new ArrayBufferWriter<byte>(4096);
        var chunk = ArrayPool<byte>.Shared.Rent(16 * 1024);
        var tooBig = false;
        try
        {
            while (true)
            {
                ValueWebSocketReceiveResult r;
                try
                {
                    r = await socket.ReceiveAsync(chunk.AsMemory(), aborted);
                }
                catch (Exception e) when (e is WebSocketException or OperationCanceledException or ObjectDisposedException)
                {
                    return;
                }
                if (r.MessageType == WebSocketMessageType.Close)
                    return;
                if (conn.IsClosing)
                    continue; // waiting for the client's close reply; ignore the rest
                if (!tooBig)
                {
                    if (message.WrittenCount + r.Count > cap)
                    {
                        tooBig = true;
                        message.Clear();
                    }
                    else
                    {
                        message.Write(chunk.AsSpan(0, r.Count));
                    }
                }
                if (!r.EndOfMessage)
                    continue;
                if (tooBig)
                {
                    conn.Close(RelayClose.TooBig, "too_big");
                    continue;
                }
                if (r.MessageType == WebSocketMessageType.Binary)
                {
                    conn.Close(RelayClose.UnsupportedData, "text_only");
                    continue;
                }
                if (!conn.OnReceived())
                {
                    log.LogWarning("relay {Role} {LogId} user {UserId} over the rate cap", Role(conn), conn.LogId, conn.UserId);
                    conn.Close(RelayClose.RateLimited, "rate_limited");
                    continue;
                }
                Dispatch(conn, message.WrittenMemory);
                message.Clear();
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(chunk);
        }
    }

    private Room RoomOf(long userId) => _rooms.GetOrAdd(userId, _ => new Room());

    // ---- membership ----

    private bool Join(RelayConnection conn)
    {
        var room = RoomOf(conn.UserId);
        lock (room)
        {
            if (conn.Role == RelayRole.Host)
            {
                if (room.Hosts.TryGetValue(conn.Id, out var old))
                {
                    room.Hosts.Remove(conn.Id);
                    old.Close(RelayClose.Replaced, "replaced");
                    log.LogInformation("relay host {LogId} replaced by {NewLogId} (user {UserId})", old.LogId, conn.LogId, conn.UserId);
                    if (old.Online)
                        BroadcastHostsLocked(room);
                }
                else if (room.Hosts.Count >= _o.MaxHostsPerUser)
                {
                    conn.Close(RelayClose.TooMany, "too_many_hosts");
                    return false;
                }
                room.Hosts[conn.Id] = conn; // online after its hello
            }
            else
            {
                if (room.Controllers.TryGetValue(conn.Id, out var old))
                {
                    // cid resume (make-before-break): hosts keep their virtual client, no joined/left.
                    room.Controllers[conn.Id] = conn;
                    old.Close(RelayClose.Replaced, "resumed");
                    if (old.Paused)
                        ToOnlineHostsLocked(room, RelayFrames.Pause(conn.Id, false));
                }
                else
                {
                    if (room.Controllers.Count >= _o.MaxControllersPerUser)
                    {
                        conn.Close(RelayClose.TooMany, "too_many_controllers");
                        return false;
                    }
                    room.Controllers[conn.Id] = conn;
                    ToOnlineHostsLocked(room, RelayFrames.Controller(conn.Id, true));
                }
                conn.Enqueue(HostsFrameLocked(room), lossy: false);
            }
        }
        log.LogInformation("relay {Role} {LogId} user {UserId} joined ({Id})", Role(conn), conn.LogId, conn.UserId, conn.Id);
        return true;
    }

    private void Leave(RelayConnection conn)
    {
        if (!_rooms.TryGetValue(conn.UserId, out var room))
            return;
        lock (room)
        {
            if (conn.Role == RelayRole.Host)
            {
                if (room.Hosts.TryGetValue(conn.Id, out var cur) && ReferenceEquals(cur, conn))
                {
                    room.Hosts.Remove(conn.Id);
                    if (conn.Online)
                        BroadcastHostsLocked(room);
                }
            }
            else if (room.Controllers.TryGetValue(conn.Id, out var cur) && ReferenceEquals(cur, conn))
            {
                room.Controllers.Remove(conn.Id);
                ToOnlineHostsLocked(room, RelayFrames.Controller(conn.Id, false));
            }
        }
    }

    private static void ToOnlineHostsLocked(Room room, ReadOnlyMemory<byte> frame)
    {
        foreach (var h in room.Hosts.Values)
            if (h.Online)
                h.Enqueue(frame, lossy: false);
    }

    private static byte[] HostsFrameLocked(Room room) => RelayFrames.Hosts(room.Known
        .OrderBy(k => k.Key, StringComparer.Ordinal)
        .Select(k => new RelayFrames.HostEntry(k.Key, k.Value.Name, k.Value.Version,
            room.Hosts.TryGetValue(k.Key, out var h) && h.Online)));

    private static void BroadcastHostsLocked(Room room)
    {
        var frame = HostsFrameLocked(room);
        foreach (var c in room.Controllers.Values)
            c.Enqueue(frame, lossy: false);
    }

    // ---- inbound frames ----

    private void Dispatch(RelayConnection conn, ReadOnlyMemory<byte> text)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(text, ParseOptions);
        }
        catch (JsonException)
        {
            conn.Enqueue(RelayFrames.Error("bad_envelope"), lossy: false);
            return;
        }
        using (doc)
        {
            var root = doc.RootElement;
            var t = root.ValueKind == JsonValueKind.Object && root.TryGetProperty("t", out var tEl) && tEl.ValueKind == JsonValueKind.String
                ? tEl.GetString()
                : null;
            switch (t)
            {
                case "ping":
                    conn.Enqueue(RelayFrames.Pong, lossy: false);
                    return;
                case "pong":
                    return;
            }
            var ok = conn.Role == RelayRole.Host ? FromHost(conn, t, root) : FromController(conn, t, root);
            if (!ok)
                conn.Enqueue(RelayFrames.Error("bad_envelope"), lossy: false);
        }
    }

    private static string? Str(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private bool FromHost(RelayConnection host, string? t, JsonElement root)
    {
        var room = RoomOf(host.UserId);
        if (t == "hello")
        {
            if (!root.TryGetProperty("host", out var info) || info.ValueKind != JsonValueKind.Object)
                return false;
            // deviceId comes from the token, never from the hello.
            var name = KeysynthAuth.Truncate(Str(info, "name") ?? "", MaxNameLength);
            var version = KeysynthAuth.Truncate(Str(info, "version") ?? "", MaxVersionLength);
            lock (room)
            {
                if (!room.Hosts.TryGetValue(host.Id, out var cur) || !ReferenceEquals(cur, host))
                    return true; // replaced meanwhile
                var first = !host.Online;
                var changed = first || host.Name != name || host.Version != version;
                host.Name = name;
                host.Version = version;
                host.Online = true;
                room.Known[host.Id] = (name, version);
                if (first) // a repeated hello only updates name/version
                {
                    foreach (var c in room.Controllers.Values)
                    {
                        host.Enqueue(RelayFrames.Controller(c.Id, true), lossy: false);
                        if (c.Paused)
                            host.Enqueue(RelayFrames.Pause(c.Id, true), lossy: false);
                    }
                }
                if (changed)
                    BroadcastHostsLocked(room);
            }
            return true;
        }
        if (t != "msg")
            return false;

        var to = Str(root, "to");
        if (to is null || !root.TryGetProperty("msg", out var msg) || msg.ValueKind != JsonValueKind.Object)
            return false;
        string? except = null;
        if (root.TryGetProperty("except", out var exEl))
        {
            if (exEl.ValueKind != JsonValueKind.String)
                return false;
            except = exEl.GetString();
        }
        var lossy = false;
        if (root.TryGetProperty("lossy", out var lEl))
        {
            if (lEl.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                return false;
            lossy = lEl.GetBoolean();
        }

        byte[]? frame = null;
        lock (room)
        {
            if (!host.Online)
            {
                host.Enqueue(RelayFrames.Error("hello_required"), lossy: false);
                return true;
            }
            if (to == "*")
            {
                foreach (var c in room.Controllers.Values)
                    if (!string.Equals(c.Id, except, StringComparison.Ordinal) && !(lossy && c.Paused))
                        c.Enqueue(frame ??= RelayFrames.FromHost(host.Id, msg), lossy);
            }
            else if (room.Controllers.TryGetValue(to, out var c) && !(lossy && c.Paused))
            {
                c.Enqueue(RelayFrames.FromHost(host.Id, msg), lossy);
            }
            // unknown cid: the controller just left (the host learns it from controller joined:false)
        }
        return true;
    }

    private bool FromController(RelayConnection ctl, string? t, JsonElement root)
    {
        var room = RoomOf(ctl.UserId);
        switch (t)
        {
            case "pause":
            case "resume":
                var paused = t == "pause";
                lock (room)
                {
                    if (ctl.Paused == paused || !room.Controllers.TryGetValue(ctl.Id, out var cur) || !ReferenceEquals(cur, ctl))
                        return true;
                    ctl.Paused = paused;
                    ToOnlineHostsLocked(room, RelayFrames.Pause(ctl.Id, paused));
                }
                return true;
            case "msg":
                var deviceId = Str(root, "host");
                if (deviceId is null || !root.TryGetProperty("msg", out var msg) || msg.ValueKind != JsonValueKind.Object)
                    return false;
                lock (room)
                {
                    if (room.Hosts.TryGetValue(deviceId, out var host) && host.Online)
                    {
                        host.Enqueue(RelayFrames.FromController(ctl.Id, msg), lossy: false);
                        return true;
                    }
                }
                JsonElement? id = msg.TryGetProperty("id", out var idEl) ? idEl : null;
                ctl.Enqueue(RelayFrames.Error("host_offline", deviceId, id), lossy: false);
                return true;
            default:
                return false;
        }
    }

    // ---- revocation ----

    /// <summary>A device row was deleted (REST): close its host socket and forget it from the hosts list.</summary>
    public void DeviceRemoved(long userId, string deviceId)
    {
        if (!_rooms.TryGetValue(userId, out var room))
            return;
        lock (room)
        {
            var known = room.Known.Remove(deviceId);
            if (room.Hosts.TryGetValue(deviceId, out var host))
            {
                room.Hosts.Remove(deviceId);
                host.Close(RelayClose.Revoked, "revoked");
                log.LogInformation("relay host {LogId} user {UserId} revoked (device deleted)", host.LogId, userId);
            }
            if (known)
                BroadcastHostsLocked(room);
        }
    }

    private async Task RevalidateAsync(RelayConnection host)
    {
        try
        {
            string? invalid;
            await using (var scope = scopes.CreateAsyncScope())
            {
                var devices = scope.ServiceProvider.GetRequiredService<IDeviceRepository>();
                var device = await devices.FindByTokenHashAsync(host.TokenHash!);
                invalid = device is null || device.UserId != host.UserId || device.DeviceId != host.Id
                    ? DeviceTokenHandler.TokenInvalid
                    : DeviceTokenHandler.Validity(device, clock.UtcNow);
            }
            if (invalid is not null)
            {
                log.LogInformation("relay host {LogId} user {UserId} revoked ({Reason})", host.LogId, host.UserId, invalid);
                var room = RoomOf(host.UserId);
                lock (room)
                {
                    if (room.Hosts.TryGetValue(host.Id, out var cur) && ReferenceEquals(cur, host))
                    {
                        room.Hosts.Remove(host.Id);
                        var forget = invalid == DeviceTokenHandler.TokenInvalid && room.Known.Remove(host.Id);
                        if (host.Online || forget)
                            BroadcastHostsLocked(room);
                    }
                }
                host.Close(RelayClose.Revoked, invalid);
            }
        }
        catch (Exception e)
        {
            // DB unavailable: keep the socket, retry at the next interval.
            log.LogWarning(e, "relay host {LogId} revalidation failed", host.LogId);
        }
        finally
        {
            Volatile.Write(ref host.Revalidating, 0);
        }
    }

    // ---- sweep ----

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        // ApplicationStopping fires before the server drains in-flight requests (= every relay socket), so the
        // close frames still go out; hosted-service StopAsync would run too late.
        using var stopping = lifetime.ApplicationStopping.Register(CloseAll);
        using var timer = new PeriodicTimer(_o.SweepInterval);
        try
        {
            while (await timer.WaitForNextTickAsync(stoppingToken))
                Sweep();
        }
        catch (OperationCanceledException)
        {
        }
    }

    internal void Sweep()
    {
        var now = Stopwatch.GetTimestamp();
        var silence = RelayConnection.Ticks(_o.SilenceTimeout);
        var ping = RelayConnection.Ticks(_o.PingInterval);
        var maxAge = RelayConnection.Ticks(_o.ControllerMaxAge);
        var revalidate = RelayConnection.Ticks(_o.HostRevalidateInterval);
        foreach (var conn in _all.Keys)
        {
            if (conn.IsClosing)
                continue;
            if (now - conn.LastRxAt > silence)
            {
                conn.Close(RelayClose.Timeout, "timeout");
                continue;
            }
            if (conn.Role == RelayRole.Controller && now - conn.OpenedAt > maxAge)
            {
                conn.Close(RelayClose.MaxAge, "max_age");
                continue;
            }
            if (now - conn.LastPingAt >= ping)
            {
                conn.LastPingAt = now;
                conn.Enqueue(RelayFrames.Ping, lossy: false);
            }
            if (conn.Role == RelayRole.Host && now >= conn.NextRevalidateAt
                && Interlocked.CompareExchange(ref conn.Revalidating, 1, 0) == 0)
            {
                conn.NextRevalidateAt = now + revalidate;
                _ = RevalidateAsync(conn);
            }
        }
    }

    private void CloseAll()
    {
        _stopping = true;
        foreach (var conn in _all.Keys)
            conn.Close(RelayClose.GoingAway, "shutdown");
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        CloseAll();
        await base.StopAsync(cancellationToken);
    }
}
