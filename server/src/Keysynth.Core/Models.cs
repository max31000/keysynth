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

/// <summary>Row of <c>presets</c>; <see cref="Patch"/> is null iff <see cref="Deleted"/>.</summary>
public sealed record PresetRow(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted, string? Patch);

public enum PresetWriteStatus { Ok, Conflict, NotFound, TooManyPresets }

/// <param name="Row">Ok: the written row. Conflict: the current row (null if there is none). Otherwise null.</param>
public sealed record PresetWriteResult(PresetWriteStatus Status, PresetRow? Row)
{
    public static PresetWriteResult Ok(PresetRow row) => new(PresetWriteStatus.Ok, row);
    public static PresetWriteResult Conflict(PresetRow? current) => new(PresetWriteStatus.Conflict, current);
    public static readonly PresetWriteResult NotFound = new(PresetWriteStatus.NotFound, null);
    public static readonly PresetWriteResult TooMany = new(PresetWriteStatus.TooManyPresets, null);
}
