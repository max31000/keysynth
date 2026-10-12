using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using Keysynth.Api.Relay;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;

namespace Keysynth.IntegrationTests.Infrastructure;

/// <summary><see cref="KeysynthFactory"/> with relay options tweaked per test (short timings, small queues).</summary>
public sealed class RelayFactory(Action<RelayOptions>? configure = null) : KeysynthFactory
{
    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        base.ConfigureWebHost(builder);
        builder.ConfigureServices(services => services.Configure<RelayOptions>(o =>
        {
            o.SweepInterval = TimeSpan.FromMilliseconds(20);
            configure?.Invoke(o);
        }));
    }

    public RelayHub Hub => Services.GetRequiredService<RelayHub>();

    /// <summary>Host socket authenticated by a device token.</summary>
    public Task<RelaySocket> HostAsync(string token, string? extraQuery = null) =>
        RelaySocket.ConnectAsync(this, "host" + extraQuery, h => h["Authorization"] = "Bearer " + token);

    /// <summary>Controller socket with the mvv42 cookie of <paramref name="telegramId"/> and the allowed Origin.</summary>
    public Task<RelaySocket> ControllerAsync(long telegramId, string? cid = null) =>
        RelaySocket.ConnectAsync(this, "controller" + (cid is null ? "" : "&cid=" + cid), h =>
        {
            h["Cookie"] = $"{Mvv42TestAuth.CookieName}={Mvv42TestAuth.CreateToken(telegramId)}";
            h["Origin"] = Mvv42TestAuth.AllowedOrigin;
        });

    /// <summary>A linked host socket that sent its hello.</summary>
    public async Task<RelaySocket> OnlineHostAsync(Linked device, string name = "MAX-PC")
    {
        var host = await HostAsync(device.Token);
        await host.SendAsync(new { t = "hello", host = new { deviceId = device.DeviceId, name, version = "0.2.0" } });
        return host;
    }
}

/// <summary>Test side of one relay WebSocket: a background reader into an inbox, close code capture.</summary>
public sealed class RelaySocket : IAsyncDisposable
{
    public static readonly TimeSpan Wait = TimeSpan.FromSeconds(5);

    private readonly Channel<JsonElement> _inbox = Channel.CreateUnbounded<JsonElement>();
    private readonly TaskCompletionSource<int> _closed = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly SemaphoreSlim _send = new(1, 1);
    private readonly Task _reader;

    private RelaySocket(WebSocket ws)
    {
        Socket = ws;
        _reader = Task.Run(ReadAsync);
    }

    public WebSocket Socket { get; }

    /// <summary>Answer server pings automatically (default) and keep them out of the inbox.</summary>
    public bool AutoPong { get; set; } = true;

    public static async Task<RelaySocket> ConnectAsync(KeysynthFactory factory, string roleQuery, Action<IDictionary<string, Microsoft.Extensions.Primitives.StringValues>> headers)
    {
        var client = factory.Server.CreateWebSocketClient();
        client.ConfigureRequest = r =>
        {
            var h = new Dictionary<string, Microsoft.Extensions.Primitives.StringValues>();
            headers(h);
            foreach (var (k, v) in h)
                r.Headers[k] = v;
        };
        var ws = await client.ConnectAsync(new Uri(factory.Server.BaseAddress, "api/relay?role=" + roleQuery), CancellationToken.None);
        return new RelaySocket(ws);
    }

    private async Task ReadAsync()
    {
        var buffer = new byte[64 * 1024];
        var message = new MemoryStream();
        try
        {
            while (true)
            {
                var r = await Socket.ReceiveAsync(buffer, CancellationToken.None);
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    _closed.TrySetResult((int?)Socket.CloseStatus ?? -1);
                    try
                    {
                        await Socket.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "", CancellationToken.None);
                    }
                    catch (Exception)
                    {
                    }
                    return;
                }
                message.Write(buffer, 0, r.Count);
                if (!r.EndOfMessage)
                    continue;
                var el = JsonDocument.Parse(message.ToArray()).RootElement.Clone();
                message.SetLength(0);
                if (AutoPong && el.TryGetProperty("t", out var t) && t.GetString() == "ping")
                {
                    _ = SendRawAsync("""{"t":"pong"}""");
                    continue;
                }
                _inbox.Writer.TryWrite(el);
            }
        }
        catch (Exception)
        {
            _closed.TrySetResult(-1); // aborted
        }
        finally
        {
            _inbox.Writer.TryComplete();
        }
    }

    public Task SendAsync(object envelope) => SendRawAsync(JsonSerializer.Serialize(envelope));

    public async Task SendRawAsync(string text)
    {
        await _send.WaitAsync();
        try
        {
            if (Socket.State == WebSocketState.Open)
                await Socket.SendAsync(Encoding.UTF8.GetBytes(text), WebSocketMessageType.Text, true, CancellationToken.None);
        }
        catch (Exception) when (_closed.Task.IsCompleted)
        {
        }
        finally
        {
            _send.Release();
        }
    }

    public async Task SendBinaryAsync(byte[] data)
    {
        await _send.WaitAsync();
        try
        {
            await Socket.SendAsync(data, WebSocketMessageType.Binary, true, CancellationToken.None);
        }
        finally
        {
            _send.Release();
        }
    }

    /// <summary>Next frame (any type).</summary>
    public async Task<JsonElement> NextAsync(TimeSpan? timeout = null)
    {
        using var cts = new CancellationTokenSource(timeout ?? Wait);
        try
        {
            return await _inbox.Reader.ReadAsync(cts.Token);
        }
        catch (OperationCanceledException)
        {
            throw new TimeoutException("no frame within " + (timeout ?? Wait));
        }
        catch (ChannelClosedException)
        {
            throw new InvalidOperationException("socket closed (" + (_closed.Task.IsCompleted ? _closed.Task.Result : 0) + ") while waiting for a frame");
        }
    }

    /// <summary>Skips frames until one with <c>t == type</c> (and the predicate) arrives.</summary>
    public async Task<JsonElement> ExpectAsync(string type, Func<JsonElement, bool>? where = null, TimeSpan? timeout = null)
    {
        var deadline = DateTime.UtcNow + (timeout ?? Wait);
        while (true)
        {
            var left = deadline - DateTime.UtcNow;
            if (left <= TimeSpan.Zero)
                throw new TimeoutException($"no '{type}' frame");
            var f = await NextAsync(left);
            if (f.GetProperty("t").GetString() == type && (where is null || where(f)))
                return f;
        }
    }

    /// <summary>True when no frame (other than auto-answered pings) arrives within <paramref name="window"/>.</summary>
    public async Task<bool> QuietAsync(TimeSpan window)
    {
        using var cts = new CancellationTokenSource(window);
        try
        {
            await _inbox.Reader.WaitToReadAsync(cts.Token);
            return !_inbox.Reader.TryPeek(out _);
        }
        catch (OperationCanceledException)
        {
            return true;
        }
    }

    /// <summary>Round trip through the server: every earlier frame of this socket has been dispatched.</summary>
    public async Task SyncAsync()
    {
        await SendAsync(new { t = "ping" });
        await ExpectAsync("pong");
    }

    /// <summary>Close code the server sent (-1 = aborted without a close frame).</summary>
    public async Task<int> ClosedAsync(TimeSpan? timeout = null)
    {
        var t = await Task.WhenAny(_closed.Task, Task.Delay(timeout ?? Wait));
        if (t != _closed.Task)
            throw new TimeoutException("socket not closed");
        return _closed.Task.Result;
    }

    public bool IsClosed => _closed.Task.IsCompleted;

    public async ValueTask DisposeAsync()
    {
        if (Socket.State == WebSocketState.Open)
        {
            try
            {
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                await Socket.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "", cts.Token);
            }
            catch (Exception)
            {
            }
        }
        await Task.WhenAny(_reader, Task.Delay(TimeSpan.FromSeconds(2)));
        Socket.Abort();
        Socket.Dispose();
    }
}
