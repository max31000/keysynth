using FluentAssertions;
using Keysynth.Core;
using Keysynth.Infrastructure;
using Keysynth.IntegrationTests.Infrastructure;
using Microsoft.Extensions.Logging.Abstractions;
using MySqlConnector;
using Testcontainers.MySql;
using Xunit;

namespace Keysynth.IntegrationTests;

/// <summary>
/// One behavioural contract for both repository implementations: the in-memory store used by the HTTP tests and the
/// MySQL one used in production (Testcontainers; skipped when no Docker daemon is reachable, always runs in CI).
/// </summary>
public abstract class RepositoryContractTests
{
    private static long _tg = 5_000_000;
    private static readonly DateTime T0 = new(2026, 10, 12, 12, 0, 0, 123, DateTimeKind.Utc);

    protected abstract IUserRepository Users { get; }
    protected abstract IDeviceRepository Devices { get; }
    protected abstract IPresetRepository Presets { get; }
    protected virtual void SkipIfUnavailable() { }

    private async Task<long> NewUser() =>
        await Users.UpsertAsync(Interlocked.Increment(ref _tg), "sub", "Max", T0);

    private static DeviceMint Mint(long userId, string? deviceId = null, string name = "PC") =>
        new(userId, deviceId ?? Guid.NewGuid().ToString("D"), name, DeviceTokens.Hash(DeviceTokens.Generate()), T0, T0.AddDays(180));

    [SkippableFact]
    public async Task UserUpsertKeepsIdAndUpdatesFields()
    {
        SkipIfUnavailable();
        var tg = Interlocked.Increment(ref _tg);
        var id = await Users.UpsertAsync(tg, "sub-1", "Old", T0);
        var again = await Users.UpsertAsync(tg, "sub-2", "Новое имя", T0.AddHours(2));
        again.Should().Be(id);
        var user = (await Users.GetAsync(id))!;
        user.Should().BeEquivalentTo(new User(id, tg, "sub-2", "Новое имя", T0, T0.AddHours(2)));
        user.LastAppOkAt.Kind.Should().Be(DateTimeKind.Utc);
        (await Users.GetAsync(-1)).Should().BeNull();
    }

    [SkippableFact]
    public async Task DeviceMintReplaceLimitAndLookup()
    {
        SkipIfUnavailable();
        var uid = await NewUser();
        var first = Mint(uid);
        (await Devices.MintAsync(first, 2)).Should().Be(MintOutcome.Created);

        var auth = (await Devices.FindByTokenHashAsync(first.TokenHash))!;
        auth.UserId.Should().Be(uid);
        auth.DeviceId.Should().Be(first.DeviceId);
        auth.TokenHash.Should().Equal(first.TokenHash);
        auth.ExpiresAt.Should().Be(T0.AddDays(180));
        auth.LastSeenAt.Should().BeNull();
        auth.LastAppOkAt.Should().Be(T0);

        var replaced = first with { TokenHash = DeviceTokens.Hash(DeviceTokens.Generate()), Name = "NEW", ExpiresAt = T0.AddDays(200) };
        (await Devices.MintAsync(replaced, 2)).Should().Be(MintOutcome.Replaced);
        (await Devices.FindByTokenHashAsync(first.TokenHash)).Should().BeNull();
        (await Devices.FindByTokenHashAsync(replaced.TokenHash)).Should().NotBeNull();
        var row = (await Devices.GetAsync(uid, first.DeviceId))!;
        row.Name.Should().Be("NEW");
        row.CreatedAt.Should().Be(T0);
        row.ExpiresAt.Should().Be(T0.AddDays(200));

        (await Devices.MintAsync(Mint(uid), 2)).Should().Be(MintOutcome.Created);
        (await Devices.MintAsync(Mint(uid), 2)).Should().Be(MintOutcome.TooManyDevices);
        (await Devices.MintAsync(replaced with { TokenHash = DeviceTokens.Hash(DeviceTokens.Generate()) }, 2))
            .Should().Be(MintOutcome.Replaced, "re-mint at the limit");

        // same deviceId under another user is a separate row
        var other = await NewUser();
        (await Devices.MintAsync(Mint(other, first.DeviceId), 2)).Should().Be(MintOutcome.Created);
        (await Devices.ListAsync(uid)).Should().HaveCount(2);
        (await Devices.ListAsync(other)).Should().ContainSingle();
    }

    [SkippableFact]
    public async Task DeviceTouchAndDelete()
    {
        SkipIfUnavailable();
        var uid = await NewUser();
        var m = Mint(uid);
        await Devices.MintAsync(m, 10);
        var id = (await Devices.FindByTokenHashAsync(m.TokenHash))!.Id;
        await Devices.TouchAsync(id, T0.AddMinutes(5));
        (await Devices.GetAsync(uid, m.DeviceId))!.LastSeenAt.Should().Be(T0.AddMinutes(5));

        (await Devices.DeleteAsync(uid + 1_000_000, m.DeviceId)).Should().BeFalse();
        (await Devices.DeleteAsync(uid, m.DeviceId)).Should().BeTrue();
        (await Devices.DeleteAsync(uid, m.DeviceId)).Should().BeFalse();
        (await Devices.FindByTokenHashAsync(m.TokenHash)).Should().BeNull();
    }

    [SkippableFact]
    public async Task PresetRevisionsAndTombstones()
    {
        SkipIfUnavailable();
        var uid = await NewUser();
        const string patch = """{"format":1,"meta":{"name":"Тёплый"}}""";

        var created = await Presets.CreateAsync(uid, "warm", patch, T0, 1000);
        created.Status.Should().Be(PresetWriteStatus.Ok);
        created.Row.Should().Be(new PresetRow("warm", 1, T0, false, patch));
        (await Presets.CreateAsync(uid, "warm", patch, T0, 1000)).Should().BeEquivalentTo(PresetWriteResult.Conflict(created.Row));

        (await Presets.UpdateAsync(uid, "warm", 2, patch, T0, default)).Status.Should().Be(PresetWriteStatus.Conflict);
        var upd = await Presets.UpdateAsync(uid, "warm", 1, """{"format":1}""", T0.AddSeconds(1));
        upd.Row.Should().Be(new PresetRow("warm", 2, T0.AddSeconds(1), false, """{"format":1}"""));

        (await Presets.DeleteAsync(uid, "warm", 1, T0)).Status.Should().Be(PresetWriteStatus.Conflict);
        (await Presets.DeleteAsync(uid, "nope", 1, T0)).Status.Should().Be(PresetWriteStatus.NotFound);
        var del = await Presets.DeleteAsync(uid, "warm", 2, T0.AddSeconds(2));
        del.Row.Should().Be(new PresetRow("warm", 3, T0.AddSeconds(2), true, null));
        (await Presets.GetAsync(uid, "warm")).Should().Be(del.Row);

        (await Presets.CreateAsync(uid, "warm", patch, T0, 1000)).Status.Should().Be(PresetWriteStatus.Conflict, "tombstone blocks create");
        var revived = await Presets.UpdateAsync(uid, "warm", 3, patch, T0.AddSeconds(3));
        revived.Row.Should().Be(new PresetRow("warm", 4, T0.AddSeconds(3), false, patch));
        (await Presets.UpdateAsync(uid, "missing", 1, patch, T0)).Should().BeEquivalentTo(PresetWriteResult.Conflict(null));
    }

    [SkippableFact]
    public async Task PresetListAndLimit()
    {
        SkipIfUnavailable();
        var uid = await NewUser();
        foreach (var slug in new[] { "b", "a-1", "a" })
            (await Presets.CreateAsync(uid, slug, """{"format":1}""", T0, 3)).Status.Should().Be(PresetWriteStatus.Ok);
        (await Presets.CreateAsync(uid, "d", """{"format":1}""", T0, 3)).Status.Should().Be(PresetWriteStatus.TooManyPresets);
        await Presets.DeleteAsync(uid, "b", 1, T0);
        (await Presets.CreateAsync(uid, "d", """{"format":1}""", T0, 3)).Status.Should().Be(PresetWriteStatus.TooManyPresets, "tombstones count");

        var list = await Presets.ListAsync(uid);
        list.Select(p => p.Slug).Should().Equal("a", "a-1", "b");
        list.Should().AllSatisfy(p => p.Patch.Should().BeNull());
        list.Single(p => p.Slug == "b").Deleted.Should().BeTrue();
        (await Presets.ListAsync(await NewUser())).Should().BeEmpty();
    }
}

public sealed class InMemoryRepositoryContractTests : RepositoryContractTests
{
    private readonly InMemoryStore _store = new();
    protected override IUserRepository Users => _store;
    protected override IDeviceRepository Devices => _store;
    protected override IPresetRepository Presets => _store;
}

/// <summary>MySQL 8.0 container with all migrations applied (pattern: bonds DatabaseFixture).</summary>
public sealed class MySqlFixture : IAsyncLifetime
{
    private MySqlContainer? _container;

    public string? ConnectionString { get; private set; }
    public string? Unavailable { get; private set; }

    public async Task InitializeAsync()
    {
        if (DockerProbe.Reason() is { } reason)
        {
            Unavailable = reason;
            return;
        }
        _container = new MySqlBuilder().WithImage("mysql:8.0").WithDatabase("keysynth_test")
            .WithUsername("keysynth").WithPassword("test-only").Build();
        await _container.StartAsync();
        ConnectionString = _container.GetConnectionString();
        var runner = new MigrationRunner(ConnectionString, NullLogger<MigrationRunner>.Instance);
        await runner.RunAsync();
        await runner.RunAsync(); // idempotent
    }

    public async Task DisposeAsync()
    {
        if (_container is not null)
            await _container.DisposeAsync();
    }
}

public sealed class MySqlRepositoryContractTests(MySqlFixture db) : RepositoryContractTests, IClassFixture<MySqlFixture>
{
    protected override void SkipIfUnavailable() => Skip.If(db.Unavailable is not null, db.Unavailable);
    protected override IUserRepository Users => new MySqlUserRepository(db.ConnectionString!);
    protected override IDeviceRepository Devices => new MySqlDeviceRepository(db.ConnectionString!);
    protected override IPresetRepository Presets => new MySqlPresetRepository(db.ConnectionString!);

    [SkippableFact]
    public async Task SchemaMigrationsRecorded()
    {
        SkipIfUnavailable();
        await using var conn = new MySqlConnection(db.ConnectionString);
        await conn.OpenAsync();
        await using var cmd = new MySqlCommand("SELECT COUNT(*) FROM schema_migrations WHERE version = 1", conn);
        Convert.ToInt32(await cmd.ExecuteScalarAsync()).Should().Be(1);
    }
}

/// <summary>Docker reachability for Testcontainers. <c>KS_TESTS_DOCKER=0|1</c> forces skip / run.</summary>
public static class DockerProbe
{
    public static string? Reason()
    {
        var force = Environment.GetEnvironmentVariable("KS_TESTS_DOCKER");
        if (force == "1")
            return null;
        if (force == "0")
            return "KS_TESTS_DOCKER=0";
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DOCKER_HOST")))
            return null;
        try
        {
            if (OperatingSystem.IsWindows())
                return Directory.GetFiles(@"\\.\pipe\").Any(p => p.EndsWith("docker_engine", StringComparison.OrdinalIgnoreCase))
                    ? null
                    : "Docker not available (no docker_engine pipe); MySQL repository tests skipped";
            return File.Exists("/var/run/docker.sock") ? null : "Docker not available (/var/run/docker.sock); MySQL repository tests skipped";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return "Docker probe failed: " + ex.Message;
        }
    }
}
