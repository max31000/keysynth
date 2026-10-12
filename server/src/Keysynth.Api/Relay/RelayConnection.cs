using System.Diagnostics;
using System.Net.WebSockets;
using System.Threading.Channels;

namespace Keysynth.Api.Relay;

public enum RelayRole { Host, Controller }

/// <summary>
/// One relay socket. The hub never writes to the socket directly: frames go through <see cref="Enqueue"/> into an
/// accounted queue drained by <see cref="RunWriterAsync"/> (the only sender, incl. the close frame), so a slow client
/// never blocks the hub or other connections. Mutable hub state (<see cref="Online"/>, <see cref="Paused"/>, name)
/// is guarded by the room lock.
/// </summary>
public sealed class RelayConnection
{
    private readonly RelayOptions _options;
    private readonly WebSocket _socket;
    private readonly Channel<ReadOnlyMemory<byte>> _out =
        Channel.CreateUnbounded<ReadOnlyMemory<byte>>(new UnboundedChannelOptions { SingleReader = true });
    private readonly CancellationTokenSource _abort = new();
    private readonly CancellationTokenRegistration _abortReg;
    private int _queuedFrames;
    private long _queuedBytes;
    private int _closing;
    private int _closeCode;
    private string _closeReason = "";
    private long _lastRxTicks;
    private long _tokens; // rate bucket, in 1/1000 messages
    private long _bucketTicks;
    private long _droppedLossy;

    public RelayConnection(RelayRole role, long userId, string id, byte[]? tokenHash, WebSocket socket, RelayOptions options)
    {
        Role = role;
        UserId = userId;
        Id = id;
        TokenHash = tokenHash;
        _socket = socket;
        _options = options;
        OpenedAt = Stopwatch.GetTimestamp();
        _lastRxTicks = OpenedAt;
        LastPingAt = OpenedAt;
        NextRevalidateAt = OpenedAt + Ticks(options.HostRevalidateInterval);
        _bucketTicks = OpenedAt;
        _tokens = RateCap * 1000L;
        // Lives until the connection is disposed: the grace abort must also fire after the close frame went out
        // (a peer that never answers the close would otherwise keep the reader waiting).
        _abortReg = _abort.Token.Register(static s => ((WebSocket)s!).Abort(), socket);
    }

    /// <summary>Invoked (on the thread pool) once when a server-initiated close starts: the hub leaves the room at
    /// once instead of when the reader finally ends.</summary>
    internal Action<RelayConnection>? Closing { get; set; }

    public RelayRole Role { get; }
    public long UserId { get; }

    /// <summary>Host: deviceId (from the token). Controller: cid.</summary>
    public string Id { get; }

    /// <summary>Host: SHA-256 of its device token (for revalidation; the token itself is never kept).</summary>
    public byte[]? TokenHash { get; }

    /// <summary>Short random id for logs (several sockets may share <see cref="Id"/> during a replace/resume).</summary>
    public string LogId { get; } = Guid.NewGuid().ToString("N")[..8];

    // Room-lock state.
    public bool Online { get; set; }
    public bool Paused { get; set; }
    public string Name { get; set; } = "";
    public string Version { get; set; } = "";

    // Sweep state (sweeper thread only).
    internal long OpenedAt { get; }
    internal long LastPingAt { get; set; }
    internal long NextRevalidateAt { get; set; }
    internal int Revalidating;

    public bool IsClosing => Volatile.Read(ref _closing) != 0;
    public int CloseCode => _closeCode;
    public long DroppedLossy => Interlocked.Read(ref _droppedLossy);
    internal long LastRxAt => Interlocked.Read(ref _lastRxTicks);

    private int RateCap => Role == RelayRole.Host ? _options.HostMessagesPerSecond : _options.ControllerMessagesPerSecond;

    public static long Ticks(TimeSpan t) => (long)(t.TotalSeconds * Stopwatch.Frequency);

    /// <summary>Reader thread: an inbound message arrived. False when it exceeds the rate cap.</summary>
    internal bool OnReceived()
    {
        var now = Stopwatch.GetTimestamp();
        Interlocked.Exchange(ref _lastRxTicks, now);
        var cap = RateCap * 1000L;
        var refill = (now - _bucketTicks) * RateCap * 1000L / Stopwatch.Frequency;
        if (refill > 0)
        {
            _tokens = Math.Min(cap, _tokens + refill);
            _bucketTicks = now;
        }
        if (_tokens < 1000)
            return false;
        _tokens -= 1000;
        return true;
    }

    /// <summary>
    /// Queues a text frame. A lossy frame is dropped while the queue is at the lossy marks (or the client paused);
    /// a non-lossy frame that would exceed the hard caps closes the connection with 4008. Never blocks.
    /// </summary>
    public void Enqueue(ReadOnlyMemory<byte> frame, bool lossy)
    {
        if (IsClosing)
            return;
        var frames = Volatile.Read(ref _queuedFrames);
        var bytes = Interlocked.Read(ref _queuedBytes);
        // Approximate under concurrent enqueuers (a few frames over the marks at most); good enough for a bound.
        if (lossy && (frames >= _options.LossyQueueFrames || bytes + frame.Length > _options.LossyQueueBytes))
        {
            Interlocked.Increment(ref _droppedLossy);
            return;
        }
        if (frames + 1 > _options.QueueFrames || bytes + frame.Length > _options.QueueBytes)
        {
            Close(RelayClose.Overflow, "overflow");
            return;
        }
        Interlocked.Increment(ref _queuedFrames);
        Interlocked.Add(ref _queuedBytes, frame.Length);
        if (!_out.Writer.TryWrite(frame))
        {
            Interlocked.Decrement(ref _queuedFrames);
            Interlocked.Add(ref _queuedBytes, -frame.Length);
        }
    }

    /// <summary>
    /// Server-initiated close (idempotent, first code wins): pending frames are discarded, the writer sends the close
    /// frame; the socket is aborted if the handshake has not finished within <see cref="RelayOptions.CloseGrace"/>.
    /// </summary>
    public void Close(int code, string reason)
    {
        if (Interlocked.CompareExchange(ref _closing, 1, 0) != 0)
            return;
        _closeCode = code;
        _closeReason = reason;
        _out.Writer.TryComplete();
        if (Closing is { } closing)
            ThreadPool.QueueUserWorkItem(static s => s.Item1(s.Item2), (closing, this), preferLocal: false);
        try
        {
            _abort.CancelAfter(_options.CloseGrace);
        }
        catch (ObjectDisposedException)
        {
        }
    }

    /// <summary>Aborts the socket now (pending receive/send fail).</summary>
    internal void Abort()
    {
        Close(RelayClose.GoingAway, "abort");
        try
        {
            _abort.Cancel();
        }
        catch (ObjectDisposedException)
        {
        }
    }

    internal async Task RunWriterAsync()
    {
        var ct = _abort.Token;
        try
        {
            await foreach (var frame in _out.Reader.ReadAllAsync(CancellationToken.None))
            {
                Interlocked.Decrement(ref _queuedFrames);
                Interlocked.Add(ref _queuedBytes, -frame.Length);
                if (IsClosing)
                    continue;
                if (_options.BeforeSendForTests is { } hook)
                    await hook(this, ct);
                if (IsClosing)
                    continue;
                await _socket.SendAsync(frame, WebSocketMessageType.Text, true, ct);
            }
            if (_socket.State is WebSocketState.Open or WebSocketState.CloseReceived)
                await _socket.CloseOutputAsync((WebSocketCloseStatus)_closeCode, _closeReason, ct);
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException or ObjectDisposedException or InvalidOperationException)
        {
            _socket.Abort();
        }
    }

    internal void DisposeAbort()
    {
        _abortReg.Dispose();
        _abort.Dispose();
    }
}
