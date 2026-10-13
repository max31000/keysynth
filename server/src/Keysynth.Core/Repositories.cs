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

/// <summary>Revisioned per-user documents with tombstones (cloud-api.md §3.3); one table per document kind.</summary>
public interface IDocumentRepository
{
    /// <summary>All rows incl. tombstones, ordered by slug (ordinal), without bodies.</summary>
    Task<IReadOnlyList<DocumentRow>> ListAsync(long userId, CancellationToken ct = default);

    Task<DocumentRow?> GetAsync(long userId, string slug, CancellationToken ct = default);

    /// <summary>Revision 1. Conflict when any row (live or tombstone) exists; TooMany at <paramref name="maxRows"/>.</summary>
    Task<DocumentWriteResult> CreateAsync(long userId, string slug, string body, DateTime now, int maxRows, CancellationToken ct = default);

    /// <summary>Update / revive when the current revision equals <paramref name="expectedRevision"/>, else Conflict.</summary>
    Task<DocumentWriteResult> UpdateAsync(long userId, string slug, uint expectedRevision, string body, DateTime now, CancellationToken ct = default);

    /// <summary>Tombstone when the revision matches; NotFound for an unknown slug; else Conflict.</summary>
    Task<DocumentWriteResult> DeleteAsync(long userId, string slug, uint expectedRevision, DateTime now, CancellationToken ct = default);
}

/// <summary>Table <c>presets</c> (body = <c>patch</c>).</summary>
public interface IPresetRepository : IDocumentRepository;

/// <summary>Table <c>songs</c> (body = <c>song</c>).</summary>
public interface ISongRepository : IDocumentRepository;
