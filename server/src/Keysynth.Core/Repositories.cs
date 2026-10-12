namespace Keysynth.Core;

public interface IUserRepository
{
    /// <summary>
    /// Inserts the user (by <paramref name="telegramId"/>) or updates <c>auth_user_id</c>, <c>name</c> and
    /// <c>last_app_ok_at</c> of the existing row. Returns <c>users.id</c>.
    /// </summary>
    Task<long> UpsertAsync(long telegramId, string authUserId, string name, DateTime now, CancellationToken ct = default);

    Task<User?> GetAsync(long id, CancellationToken ct = default);
}

public interface IDeviceRepository
{
    /// <summary>
    /// Atomically (per user): replaces the token of the existing (user, deviceId) row → <see cref="MintOutcome.Replaced"/>;
    /// else inserts a row unless the user already has <paramref name="maxDevices"/> → Created / TooManyDevices.
    /// </summary>
    Task<MintOutcome> MintAsync(DeviceMint mint, int maxDevices, CancellationToken ct = default);

    Task<IReadOnlyList<Device>> ListAsync(long userId, CancellationToken ct = default);
    Task<Device?> GetAsync(long userId, string deviceId, CancellationToken ct = default);
    Task<DeviceAuth?> FindByTokenHashAsync(byte[] tokenHash, CancellationToken ct = default);
    Task<bool> DeleteAsync(long userId, string deviceId, CancellationToken ct = default);
    Task TouchAsync(long id, DateTime now, CancellationToken ct = default);
}

public interface IPresetRepository
{
    /// <summary>All rows incl. tombstones, ordered by slug (ordinal).</summary>
    Task<IReadOnlyList<PresetRow>> ListAsync(long userId, CancellationToken ct = default);

    Task<PresetRow?> GetAsync(long userId, string slug, CancellationToken ct = default);

    /// <summary>Revision 1. Conflict when any row (live or tombstone) exists; TooManyPresets at the limit.</summary>
    Task<PresetWriteResult> CreateAsync(long userId, string slug, string patch, DateTime now, int maxPresets, CancellationToken ct = default);

    /// <summary>Update / revive when the current revision equals <paramref name="expectedRevision"/>, else Conflict.</summary>
    Task<PresetWriteResult> UpdateAsync(long userId, string slug, uint expectedRevision, string patch, DateTime now, CancellationToken ct = default);

    /// <summary>Tombstone when the revision matches; NotFound for an unknown slug; else Conflict.</summary>
    Task<PresetWriteResult> DeleteAsync(long userId, string slug, uint expectedRevision, DateTime now, CancellationToken ct = default);
}
