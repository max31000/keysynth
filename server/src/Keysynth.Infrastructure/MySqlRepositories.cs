using Dapper;
using Keysynth.Core;
using MySqlConnector;

namespace Keysynth.Infrastructure;

/// <summary>Dapper + MySqlConnector implementations of the Core repositories (schema: Migrations/001_initial.sql).</summary>
public abstract class MySqlRepository(string connectionString)
{
    // Ids are CHAR(36) mapped to string; MySqlConnector would otherwise read CHAR(36) columns as Guid.
    private readonly string _connectionString =
        new MySqlConnectionStringBuilder(connectionString) { GuidFormat = MySqlGuidFormat.None }.ConnectionString;

    protected async Task<MySqlConnection> OpenAsync(CancellationToken ct)
    {
        var conn = new MySqlConnection(_connectionString);
        await conn.OpenAsync(ct);
        return conn;
    }

    protected static CommandDefinition Cmd(string sql, object? args, CancellationToken ct, MySqlTransaction? tx = null) =>
        new(sql, args, tx, cancellationToken: ct);

    /// <summary>Serialises writes of one user (devices / presets limits) on the users row.</summary>
    protected static Task LockUserAsync(MySqlConnection conn, MySqlTransaction tx, long userId, CancellationToken ct) =>
        conn.ExecuteScalarAsync<long?>(Cmd("SELECT id FROM users WHERE id = @userId FOR UPDATE", new { userId }, ct, tx));
}

public sealed class MySqlUserRepository(string connectionString) : MySqlRepository(connectionString), IUserRepository
{
    public async Task<long> UpsertAsync(long telegramId, string authUserId, string name, DateTime now, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        // id = LAST_INSERT_ID(id) makes LAST_INSERT_ID() return the existing row's id on the update path.
        return await conn.ExecuteScalarAsync<long>(Cmd("""
            INSERT INTO users (telegram_id, auth_user_id, name, created_at, last_app_ok_at)
            VALUES (@telegramId, @authUserId, @name, @now, @now)
            ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id), auth_user_id = VALUES(auth_user_id),
                                    name = VALUES(name), last_app_ok_at = VALUES(last_app_ok_at);
            SELECT LAST_INSERT_ID();
            """, new { telegramId, authUserId, name, now }, ct));
    }

    public async Task<User?> GetAsync(long id, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        var row = await conn.QuerySingleOrDefaultAsync<UserRow>(Cmd(
            "SELECT id, telegram_id, auth_user_id, name, created_at, last_app_ok_at FROM users WHERE id = @id", new { id }, ct));
        return row is null
            ? null
            : new User(row.id, row.telegram_id, row.auth_user_id, row.name, Time.AsUtc(row.created_at), Time.AsUtc(row.last_app_ok_at));
    }

    private sealed class UserRow
    {
        public long id { get; set; }
        public long telegram_id { get; set; }
        public string auth_user_id { get; set; } = "";
        public string name { get; set; } = "";
        public DateTime created_at { get; set; }
        public DateTime last_app_ok_at { get; set; }
    }
}

public sealed class MySqlDeviceRepository(string connectionString) : MySqlRepository(connectionString), IDeviceRepository
{
    private const string Columns = "id, user_id, device_id, name, created_at, expires_at, last_seen_at";

    public async Task<MintOutcome> MintAsync(DeviceMint mint, int maxDevices, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        await using var tx = await conn.BeginTransactionAsync(ct);
        await LockUserAsync(conn, tx, mint.UserId, ct);

        var updated = await conn.ExecuteAsync(Cmd("""
            UPDATE devices SET name = @Name, token_hash = @TokenHash, expires_at = @ExpiresAt
            WHERE user_id = @UserId AND device_id = @DeviceId
            """, mint, ct, tx));
        MintOutcome outcome;
        if (updated > 0)
        {
            outcome = MintOutcome.Replaced;
        }
        else
        {
            var count = await conn.ExecuteScalarAsync<long>(Cmd(
                "SELECT COUNT(*) FROM devices WHERE user_id = @UserId", mint, ct, tx));
            if (count >= maxDevices)
            {
                await tx.RollbackAsync(ct);
                return MintOutcome.TooManyDevices;
            }
            await conn.ExecuteAsync(Cmd("""
                INSERT INTO devices (user_id, device_id, name, token_hash, created_at, expires_at, last_seen_at)
                VALUES (@UserId, @DeviceId, @Name, @TokenHash, @Now, @ExpiresAt, NULL)
                """, mint, ct, tx));
            outcome = MintOutcome.Created;
        }
        await tx.CommitAsync(ct);
        return outcome;
    }

    public async Task<IReadOnlyList<Device>> ListAsync(long userId, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        var rows = await conn.QueryAsync<DeviceRow>(Cmd(
            $"SELECT {Columns} FROM devices WHERE user_id = @userId ORDER BY created_at, id", new { userId }, ct));
        return rows.Select(r => r.ToDevice()).ToList();
    }

    public async Task<Device?> GetAsync(long userId, string deviceId, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        var row = await conn.QuerySingleOrDefaultAsync<DeviceRow>(Cmd(
            $"SELECT {Columns} FROM devices WHERE user_id = @userId AND device_id = @deviceId", new { userId, deviceId }, ct));
        return row?.ToDevice();
    }

    public async Task<DeviceAuth?> FindByTokenHashAsync(byte[] tokenHash, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        var row = await conn.QuerySingleOrDefaultAsync<DeviceAuthRow>(Cmd("""
            SELECT d.id, d.user_id, d.device_id, d.token_hash, d.expires_at, d.last_seen_at, u.last_app_ok_at
            FROM devices d JOIN users u ON u.id = d.user_id
            WHERE d.token_hash = @tokenHash
            """, new { tokenHash }, ct));
        return row is null
            ? null
            : new DeviceAuth(row.id, row.user_id, row.device_id, row.token_hash, Time.AsUtc(row.expires_at),
                row.last_seen_at is { } seen ? Time.AsUtc(seen) : null, Time.AsUtc(row.last_app_ok_at));
    }

    public async Task<bool> DeleteAsync(long userId, string deviceId, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        return await conn.ExecuteAsync(Cmd(
            "DELETE FROM devices WHERE user_id = @userId AND device_id = @deviceId", new { userId, deviceId }, ct)) > 0;
    }

    public async Task TouchAsync(long id, DateTime now, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        await conn.ExecuteAsync(Cmd("UPDATE devices SET last_seen_at = @now WHERE id = @id", new { id, now }, ct));
    }

    private class DeviceRow
    {
        public long id { get; set; }
        public long user_id { get; set; }
        public string device_id { get; set; } = "";
        public string name { get; set; } = "";
        public DateTime created_at { get; set; }
        public DateTime expires_at { get; set; }
        public DateTime? last_seen_at { get; set; }

        public Device ToDevice() => new(id, user_id, device_id, name, Time.AsUtc(created_at), Time.AsUtc(expires_at),
            last_seen_at is { } seen ? Time.AsUtc(seen) : null);
    }

    private sealed class DeviceAuthRow
    {
        public long id { get; set; }
        public long user_id { get; set; }
        public string device_id { get; set; } = "";
        public byte[] token_hash { get; set; } = [];
        public DateTime expires_at { get; set; }
        public DateTime? last_seen_at { get; set; }
        public DateTime last_app_ok_at { get; set; }
    }
}

public sealed class MySqlPresetRepository(string connectionString) : MySqlRepository(connectionString), IPresetRepository
{
    private const string Columns = "slug, revision, updated_at, deleted, patch";

    public async Task<IReadOnlyList<PresetRow>> ListAsync(long userId, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        // Meta only; ascii_bin collation = ordinal order.
        var rows = await conn.QueryAsync<Row>(Cmd(
            "SELECT slug, revision, updated_at, deleted, NULL AS patch FROM presets WHERE user_id = @userId ORDER BY slug",
            new { userId }, ct));
        return rows.Select(r => r.ToPreset()).ToList();
    }

    public async Task<PresetRow?> GetAsync(long userId, string slug, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        return await GetAsync(conn, null, userId, slug, ct);
    }

    public async Task<PresetWriteResult> CreateAsync(long userId, string slug, string patch, DateTime now, int maxPresets, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        await using var tx = await conn.BeginTransactionAsync(ct);
        await LockUserAsync(conn, tx, userId, ct);

        if (await GetAsync(conn, tx, userId, slug, ct) is { } existing)
        {
            await tx.RollbackAsync(ct);
            return PresetWriteResult.Conflict(existing);
        }
        var count = await conn.ExecuteScalarAsync<long>(Cmd(
            "SELECT COUNT(*) FROM presets WHERE user_id = @userId", new { userId }, ct, tx));
        if (count >= maxPresets)
        {
            await tx.RollbackAsync(ct);
            return PresetWriteResult.TooMany;
        }
        try
        {
            await conn.ExecuteAsync(Cmd("""
                INSERT INTO presets (user_id, slug, revision, updated_at, deleted, patch)
                VALUES (@userId, @slug, 1, @now, 0, @patch)
                """, new { userId, slug, now, patch }, ct, tx));
        }
        catch (MySqlException ex) when (ex.ErrorCode == MySqlErrorCode.DuplicateKeyEntry)
        {
            await tx.RollbackAsync(ct);
            return PresetWriteResult.Conflict(await GetAsync(userId, slug, ct));
        }
        await tx.CommitAsync(ct);
        return PresetWriteResult.Ok(new PresetRow(slug, 1, now, false, patch));
    }

    public async Task<PresetWriteResult> UpdateAsync(long userId, string slug, uint expectedRevision, string patch, DateTime now, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        var affected = await conn.ExecuteAsync(Cmd("""
            UPDATE presets SET revision = revision + 1, updated_at = @now, deleted = 0, patch = @patch
            WHERE user_id = @userId AND slug = @slug AND revision = @expectedRevision
            """, new { userId, slug, expectedRevision, now, patch }, ct));
        return affected > 0
            ? PresetWriteResult.Ok(new PresetRow(slug, expectedRevision + 1, now, false, patch))
            : PresetWriteResult.Conflict(await GetAsync(conn, null, userId, slug, ct));
    }

    public async Task<PresetWriteResult> DeleteAsync(long userId, string slug, uint expectedRevision, DateTime now, CancellationToken ct = default)
    {
        await using var conn = await OpenAsync(ct);
        var affected = await conn.ExecuteAsync(Cmd("""
            UPDATE presets SET revision = revision + 1, updated_at = @now, deleted = 1, patch = NULL
            WHERE user_id = @userId AND slug = @slug AND revision = @expectedRevision
            """, new { userId, slug, expectedRevision, now }, ct));
        if (affected > 0)
            return PresetWriteResult.Ok(new PresetRow(slug, expectedRevision + 1, now, true, null));
        var current = await GetAsync(conn, null, userId, slug, ct);
        return current is null ? PresetWriteResult.NotFound : PresetWriteResult.Conflict(current);
    }

    private static async Task<PresetRow?> GetAsync(MySqlConnection conn, MySqlTransaction? tx, long userId, string slug, CancellationToken ct)
    {
        var row = await conn.QuerySingleOrDefaultAsync<Row>(Cmd(
            $"SELECT {Columns} FROM presets WHERE user_id = @userId AND slug = @slug", new { userId, slug }, ct, tx));
        return row?.ToPreset();
    }

    private sealed class Row
    {
        public string slug { get; set; } = "";
        public uint revision { get; set; }
        public DateTime updated_at { get; set; }
        public bool deleted { get; set; }
        public string? patch { get; set; }

        public PresetRow ToPreset() => new(slug, revision, Time.AsUtc(updated_at), deleted, deleted ? null : patch);
    }
}
