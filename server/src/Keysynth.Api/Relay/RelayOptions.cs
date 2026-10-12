namespace Keysynth.Api.Relay;

/// <summary>
/// Relay hub limits and timings (docs/PROTOCOL.md *Relay*, spec docs/specs/2026-10-12-remote.md §10). Bound from the
/// <c>Relay</c> configuration section; the defaults are the contract, tests shorten the timings.
/// </summary>
public sealed class RelayOptions
{
    public int MaxHostsPerUser { get; set; } = 4;
    public int MaxControllersPerUser { get; set; } = 8;

    /// <summary>Largest accepted inbound message (all fragments) per role; bigger → close 1009.</summary>
    public int HostFrameBytes { get; set; } = 1024 * 1024;
    public int ControllerFrameBytes { get; set; } = 128 * 1024;

    /// <summary>Inbound safety caps (token bucket, burst = one second); exceeded → close 4029.</summary>
    public int HostMessagesPerSecond { get; set; } = 300;
    public int ControllerMessagesPerSecond { get; set; } = 100;

    /// <summary>App-level <c>{t:"ping"}</c> from the server; any inbound frame counts as liveness.</summary>
    public TimeSpan PingInterval { get; set; } = TimeSpan.FromSeconds(20);
    public TimeSpan SilenceTimeout { get; set; } = TimeSpan.FromSeconds(45);

    /// <summary>Controller sockets live at most this long (clients reconnect with the same cid before) → close 4002.</summary>
    public TimeSpan ControllerMaxAge { get; set; } = TimeSpan.FromMinutes(60);

    /// <summary>Host device tokens are re-checked (exists, not expired, <c>last_app_ok_at</c>) this often → close 4003.</summary>
    public TimeSpan HostRevalidateInterval { get; set; } = TimeSpan.FromMinutes(5);

    /// <summary>Granularity of the heartbeat / age / revalidation sweep.</summary>
    public TimeSpan SweepInterval { get; set; } = TimeSpan.FromSeconds(1);

    /// <summary>Outgoing queue per connection: lossy frames are dropped at or above the lossy marks; a non-lossy frame
    /// that would exceed the hard caps closes the connection with 4008.</summary>
    public int QueueFrames { get; set; } = 1024;
    public long QueueBytes { get; set; } = 8L * 1024 * 1024;
    public int LossyQueueFrames { get; set; } = 64;
    public long LossyQueueBytes { get; set; } = 1024 * 1024;

    /// <summary>After a server-initiated close, the socket is aborted if the close handshake has not finished.</summary>
    public TimeSpan CloseGrace { get; set; } = TimeSpan.FromSeconds(5);

    /// <summary>Tests only: awaited by a connection's writer before each send (simulates a slow client).</summary>
    public Func<RelayConnection, CancellationToken, Task>? BeforeSendForTests { get; set; }
}

/// <summary>Close codes the hub sends (docs/PROTOCOL.md *Relay*).</summary>
public static class RelayClose
{
    public const int Normal = 1000;
    public const int GoingAway = 1001;       // server shutdown
    public const int UnsupportedData = 1003; // binary frame
    public const int TooBig = 1009;          // inbound message over the role's frame cap
    public const int Replaced = 4000;        // same host deviceId / controller cid connected again
    public const int Timeout = 4001;         // no frame for SilenceTimeout
    public const int MaxAge = 4002;          // controller socket older than ControllerMaxAge
    public const int Revoked = 4003;         // host device deleted / token expired / reauth required
    public const int Overflow = 4008;        // non-lossy frame did not fit the outgoing queue
    public const int TooMany = 4009;         // over MaxHostsPerUser / MaxControllersPerUser
    public const int RateLimited = 4029;     // over the inbound message rate cap
}
