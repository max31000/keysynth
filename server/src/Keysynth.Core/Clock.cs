namespace Keysynth.Core;

/// <summary>
/// Server time for everything stored or compared (expiry, last_app_ok_at, updatedAt). Always UTC and truncated to
/// milliseconds, so values round-trip through DATETIME(3) unchanged. Not <see cref="TimeProvider"/>: replacing that in
/// tests would also move the clocks of the auth / rate-limit middleware.
/// </summary>
public interface IClock
{
    DateTime UtcNow { get; }
}

public sealed class SystemClock : IClock
{
    public DateTime UtcNow => Time.TruncateToMs(DateTime.UtcNow);
}

public static class Time
{
    public static DateTime TruncateToMs(DateTime t) =>
        new(t.Ticks - t.Ticks % TimeSpan.TicksPerMillisecond, DateTimeKind.Utc);

    /// <summary>MySqlConnector returns <see cref="DateTimeKind.Unspecified"/>; all stored values are UTC.</summary>
    public static DateTime AsUtc(DateTime t) => DateTime.SpecifyKind(t, DateTimeKind.Utc);
}
