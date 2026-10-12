using Keysynth.Core;

namespace Keysynth.IntegrationTests.Infrastructure;

/// <summary>
/// In-memory implementation of the three repositories with the MySQL semantics (unique keys, conditional updates,
/// per-user limits). Used by the WebApplicationFactory tests when no Docker is available; its behaviour is pinned to the
/// MySQL implementation by <see cref="RepositoryContractTests"/>, which runs against both.
/// </summary>
public sealed class InMemoryStore : IUserRepository, IDeviceRepository, IPresetRepository
{
    private readonly object _gate = new();
    private readonly List<User> _users = [];
    private readonly List<DeviceRecord> _devices = [];
    private readonly Dictionary<(long, string), PresetRow> _presets = [];
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

    // ---- presets
    Task<IReadOnlyList<PresetRow>> IPresetRepository.ListAsync(long userId, CancellationToken ct)
    {
        lock (_gate)
            return Task.FromResult<IReadOnlyList<PresetRow>>(_presets.Where(p => p.Key.Item1 == userId)
                .Select(p => p.Value with { Patch = null })
                .OrderBy(p => p.Slug, StringComparer.Ordinal).ToList());
    }

    public Task<PresetRow?> GetAsync(long userId, string slug, CancellationToken ct = default)
    {
        lock (_gate)
            return Task.FromResult(_presets.GetValueOrDefault((userId, slug)));
    }

    public Task<PresetWriteResult> CreateAsync(long userId, string slug, string patch, DateTime now, int maxPresets, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (_presets.TryGetValue((userId, slug), out var existing))
                return Task.FromResult(PresetWriteResult.Conflict(existing));
            if (_presets.Keys.Count(k => k.Item1 == userId) >= maxPresets)
                return Task.FromResult(PresetWriteResult.TooMany);
            var row = new PresetRow(slug, 1, now, false, patch);
            _presets[(userId, slug)] = row;
            return Task.FromResult(PresetWriteResult.Ok(row));
        }
    }

    public Task<PresetWriteResult> UpdateAsync(long userId, string slug, uint expectedRevision, string patch, DateTime now, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (!_presets.TryGetValue((userId, slug), out var cur) || cur.Revision != expectedRevision)
                return Task.FromResult(PresetWriteResult.Conflict(cur));
            var row = new PresetRow(slug, cur.Revision + 1, now, false, patch);
            _presets[(userId, slug)] = row;
            return Task.FromResult(PresetWriteResult.Ok(row));
        }
    }

    public Task<PresetWriteResult> DeleteAsync(long userId, string slug, uint expectedRevision, DateTime now, CancellationToken ct = default)
    {
        lock (_gate)
        {
            if (!_presets.TryGetValue((userId, slug), out var cur))
                return Task.FromResult(PresetWriteResult.NotFound);
            if (cur.Revision != expectedRevision)
                return Task.FromResult(PresetWriteResult.Conflict(cur));
            var row = new PresetRow(slug, cur.Revision + 1, now, true, null);
            _presets[(userId, slug)] = row;
            return Task.FromResult(PresetWriteResult.Ok(row));
        }
    }

    /// <summary>Test hook: seeds tombstones directly (limit tests without 1000 HTTP writes).</summary>
    public void SeedTombstones(long userId, int count, DateTime now)
    {
        lock (_gate)
            for (var i = 0; i < count; i++)
                _presets[(userId, $"seed-{i}")] = new PresetRow($"seed-{i}", 2, now, true, null);
    }
}
