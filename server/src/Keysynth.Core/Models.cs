namespace Keysynth.Core;

/// <summary>Row of <c>users</c> (local user, provisioned by Telegram id).</summary>
public sealed record User(long Id, long TelegramId, string AuthUserId, string Name, DateTime CreatedAt, DateTime LastAppOkAt);

/// <summary>Row of <c>devices</c> without the token hash.</summary>
public sealed record Device(long Id, long UserId, string DeviceId, string Name, DateTime CreatedAt, DateTime ExpiresAt, DateTime? LastSeenAt);

/// <summary>A device row found by token hash, joined with the owner's <c>last_app_ok_at</c>.</summary>
public sealed record DeviceAuth(
    long Id, long UserId, string DeviceId, byte[] TokenHash, DateTime ExpiresAt, DateTime? LastSeenAt, DateTime LastAppOkAt);

/// <summary>Input of <see cref="IDeviceRepository.MintAsync"/>.</summary>
public sealed record DeviceMint(long UserId, string DeviceId, string Name, byte[] TokenHash, DateTime Now, DateTime ExpiresAt);

public enum MintOutcome { Created, Replaced, TooManyDevices }

/// <summary>
/// Row of a revisioned per-user document table (<c>presets</c>, <c>songs</c>; cloud-api.md §3.3 / §3.3a).
/// <see cref="Body"/> (the <c>patch</c> / <c>song</c> column) is null iff <see cref="Deleted"/>.
/// </summary>
public sealed record DocumentRow(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted, string? Body);

public enum DocumentWriteStatus { Ok, Conflict, NotFound, TooMany }

/// <param name="Row">Ok: the written row. Conflict: the current row (null if there is none). Otherwise null.</param>
public sealed record DocumentWriteResult(DocumentWriteStatus Status, DocumentRow? Row)
{
    public static DocumentWriteResult Ok(DocumentRow row) => new(DocumentWriteStatus.Ok, row);
    public static DocumentWriteResult Conflict(DocumentRow? current) => new(DocumentWriteStatus.Conflict, current);
    public static readonly DocumentWriteResult NotFound = new(DocumentWriteStatus.NotFound, null);
    public static readonly DocumentWriteResult TooMany = new(DocumentWriteStatus.TooMany, null);
}
