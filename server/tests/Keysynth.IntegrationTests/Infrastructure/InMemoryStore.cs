using Keysynth.Core;

namespace Keysynth.IntegrationTests.Infrastructure;

/// <summary>
/// In-memory implementation of the repositories with the MySQL semantics (unique keys, conditional updates,
/// per-user limits). Used by the WebApplicationFactory tests when no Docker is available; its behaviour is pinned to the
/// MySQL implementation by <see cref="RepositoryContractTests"/>, which runs against both.
/// </summary>
public sealed class InMemoryStore : IUserRepository, IDeviceRepository
{
    private readonly object _gate = new();
    private readonly List<User> _users = [];
    private readonly List<DeviceRecord> _devices = [];
    private long _nextUserId = 1, _nextDeviceId = 1;

    private sealed record DeviceRecord(Device Device, byte[] TokenHash);

    // ---- users
    public Task<long> UpsertAsync(long telegramId, string authUserId, string name, DateTime now, CancellationToken ct = default)
    {
        lock (_gate)
        {
            var i = _users.FindIndex(u => u.TelegramId == telegramId);
            if (i >= 0)
            {
                _users[i] = _users[i] with { AuthUserId = authUserId, Name = name, LastAppOkAt = now };
                return Task.FromResult(_users[i].Id);
            }
            var user = new User(_nextUserId++, telegramId, authUserId, name, now, now);
            _users.Add(user);
            return Task.FromResult(user.Id);
        }
    }

    public Task<User?> GetAsync(long id, CancellationToken ct = default)
    {
        lock (_gate)
            return Task.FromResult(_users.FirstOrDefault(u => u.Id == id));
    }

    /// <summary>Test hook: rewinds a user's <c>last_app_ok_at</c>.</summary>
    public void SetLastAppOk(long userId, DateTime at)
    {
        lock (_gate)
        {
            var i = _users.FindIndex(u => u.Id == userId);
            _users[i] = _users[i] with { LastAppOkAt = at };
        }
    }

    // ---- devices
    public Task<MintOutcome> MintAsync(DeviceMint m, int maxDevices, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (_devices.Any(d => d.TokenHash.AsSpan().SequenceEqual(m.TokenHash)))
                throw new InvalidOperationException("duplicate token hash (ux_devices_token)");
            var i = _devices.FindIndex(d => d.Device.UserId == m.UserId && d.Device.DeviceId == m.DeviceId);
            if (i >= 0)
            {
                _devices[i] = new DeviceRecord(_devices[i].Device with { Name = m.Name, ExpiresAt = m.ExpiresAt }, m.TokenHash);
                return Task.FromResult(MintOutcome.Replaced);
            }
            if (_devices.Count(d => d.Device.UserId == m.UserId) >= maxDevices)
                return Task.FromResult(MintOutcome.TooManyDevices);
            _devices.Add(new DeviceRecord(new Device(_nextDeviceId++, m.UserId, m.DeviceId, m.Name, m.Now, m.ExpiresAt, null), m.TokenHash));
            return Task.FromResult(MintOutcome.Created);
        }
    }

    public Task<IReadOnlyList<Device>> ListAsync(long userId, CancellationToken ct = default)
    {
        lock (_gate)
            return Task.FromResult<IReadOnlyList<Device>>(_devices.Select(d => d.Device).Where(d => d.UserId == userId)
                .OrderBy(d => d.CreatedAt).ThenBy(d => d.Id).ToList());
    }

    Task<Device?> IDeviceRepository.GetAsync(long userId, string deviceId, CancellationToken ct)
    {
        lock (_gate)
            return Task.FromResult(_devices.Select(d => d.Device).FirstOrDefault(d => d.UserId == userId && d.DeviceId == deviceId));
    }

    public Task<DeviceAuth?> FindByTokenHashAsync(byte[] tokenHash, CancellationToken ct = default)
    {
        lock (_gate)
        {
            var r = _devices.FirstOrDefault(d => d.TokenHash.AsSpan().SequenceEqual(tokenHash));
            if (r is null)
                return Task.FromResult<DeviceAuth?>(null);
            var user = _users.First(u => u.Id == r.Device.UserId);
            return Task.FromResult<DeviceAuth?>(new DeviceAuth(r.Device.Id, r.Device.UserId, r.Device.DeviceId, r.TokenHash.ToArray(),
                r.Device.ExpiresAt, r.Device.LastSeenAt, user.LastAppOkAt));
        }
    }

    public Task<bool> DeleteAsync(long userId, string deviceId, CancellationToken ct = default)
    {
        lock (_gate)
            return Task.FromResult(_devices.RemoveAll(d => d.Device.UserId == userId && d.Device.DeviceId == deviceId) > 0);
    }

    public Task TouchAsync(long id, DateTime now, CancellationToken ct = default)
    {
        lock (_gate)
        {
            var i = _devices.FindIndex(d => d.Device.Id == id);
            if (i >= 0)
                _devices[i] = _devices[i] with { Device = _devices[i].Device with { LastSeenAt = now } };
            TouchCount++;
        }
        return Task.CompletedTask;
    }

    /// <summary>Test hook: number of last_seen_at writes.</summary>
    public int TouchCount { get; private set; }

    // ---- presets / songs
    public InMemoryDocuments Presets { get; } = new();
    public InMemoryDocuments Songs { get; } = new();
}

/// <summary>One revisioned document table (<c>presets</c> or <c>songs</c>) with the MySQL semantics.</summary>
public sealed class InMemoryDocuments : IPresetRepository, ISongRepository
{
    private readonly object _gate = new();
    private readonly Dictionary<(long, string), DocumentRow> _rows = [];

    public Task<IReadOnlyList<DocumentRow>> ListAsync(long userId, CancellationToken ct = default)
    {
        lock (_gate)
            return Task.FromResult<IReadOnlyList<DocumentRow>>(_rows.Where(p => p.Key.Item1 == userId)
                .Select(p => p.Value with { Body = null })
                .OrderBy(p => p.Slug, StringComparer.Ordinal).ToList());
    }

    public Task<DocumentRow?> GetAsync(long userId, string slug, CancellationToken ct = default)
    {
        lock (_gate)
            return Task.FromResult(_rows.GetValueOrDefault((userId, slug)));
    }

    public Task<DocumentWriteResult> CreateAsync(long userId, string slug, string body, DateTime now, int maxRows, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (_rows.TryGetValue((userId, slug), out var existing))
                return Task.FromResult(DocumentWriteResult.Conflict(existing));
            if (_rows.Keys.Count(k => k.Item1 == userId) >= maxRows)
                return Task.FromResult(DocumentWriteResult.TooMany);
            var row = new DocumentRow(slug, 1, now, false, body);
            _rows[(userId, slug)] = row;
            return Task.FromResult(DocumentWriteResult.Ok(row));
        }
    }

    public Task<DocumentWriteResult> UpdateAsync(long userId, string slug, uint expectedRevision, string body, DateTime now, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (!_rows.TryGetValue((userId, slug), out var cur) || cur.Revision != expectedRevision)
                return Task.FromResult(DocumentWriteResult.Conflict(cur));
            var row = new DocumentRow(slug, cur.Revision + 1, now, false, body);
            _rows[(userId, slug)] = row;
            return Task.FromResult(DocumentWriteResult.Ok(row));
        }
    }

    public Task<DocumentWriteResult> DeleteAsync(long userId, string slug, uint expectedRevision, DateTime now, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (!_rows.TryGetValue((userId, slug), out var cur))
                return Task.FromResult(DocumentWriteResult.NotFound);
            if (cur.Revision != expectedRevision)
                return Task.FromResult(DocumentWriteResult.Conflict(cur));
            var row = new DocumentRow(slug, cur.Revision + 1, now, true, null);
            _rows[(userId, slug)] = row;
            return Task.FromResult(DocumentWriteResult.Ok(row));
        }
    }

    /// <summary>Test hook: seeds tombstones directly (limit tests without 1000 HTTP writes).</summary>
    public void SeedTombstones(long userId, int count, DateTime now)
    {
        lock (_gate)
            for (var i = 0; i < count; i++)
                _rows[(userId, $"seed-{i}")] = new DocumentRow($"seed-{i}", 2, now, true, null);
    }
}
