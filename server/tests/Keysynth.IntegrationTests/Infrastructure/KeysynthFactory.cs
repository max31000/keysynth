using System.Net.Http.Json;
using System.Text.Json;
using Keysynth.Core;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;

namespace Keysynth.IntegrationTests.Infrastructure;

public sealed class FakeClock(DateTime start) : IClock
{
    private DateTime _now = Time.TruncateToMs(start);
    public DateTime UtcNow => _now;
    public void Advance(TimeSpan by) => _now = Time.TruncateToMs(_now + by);
}

/// <summary>
/// The real API host (environment Testing: no migrations) with the in-memory repositories, a controllable clock,
/// the stub JWKS key and a temporary samples root. One factory = one isolated world (store, clock, rate limiters).
/// </summary>
public class KeysynthFactory : WebApplicationFactory<Program>
{
    public static readonly DateTime Start = new(2026, 10, 12, 12, 0, 0, DateTimeKind.Utc);

    public InMemoryStore Store { get; } = new();
    public FakeClock Clock { get; } = new(Start);
    public string SamplesRoot { get; } = Directory.CreateTempSubdirectory("ks-samples-").FullName;
    public bool DevServeFiles { get; init; }

    private long _nextTelegramId = 1_000_000;

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        builder.UseEnvironment("Testing");
        builder.ConfigureAppConfiguration((_, config) => config.AddInMemoryCollection(new Dictionary<string, string?>
        {
            // Never opened: the repositories are replaced below. Unroutable on purpose.
            ["ConnectionStrings:DefaultConnection"] = "Server=127.0.0.1;Port=1;Database=none;User=none;Password=none;",
            ["Keysynth:SamplesRoot"] = SamplesRoot,
            ["KS_DEV_SERVE_FILES"] = DevServeFiles ? "1" : "0",
        }));
        builder.ConfigureServices(services =>
        {
            services.RemoveAll<IUserRepository>();
            services.RemoveAll<IDeviceRepository>();
            services.RemoveAll<IPresetRepository>();
            services.RemoveAll<ISongRepository>();
            services.RemoveAll<IClock>();
            services.AddSingleton<IUserRepository>(Store);
            services.AddSingleton<IDeviceRepository>(Store);
            services.AddSingleton<IPresetRepository>(Store.Presets);
            services.AddSingleton<ISongRepository>(Store.Songs);
            services.AddSingleton<IClock>(Clock);
            Mvv42TestAuth.UseTestSigningKey(services);
        });
    }

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing && Directory.Exists(SamplesRoot))
            Directory.Delete(SamplesRoot, recursive: true);
    }

    public long NewTelegramId() => Interlocked.Increment(ref _nextTelegramId);

    /// <summary>A client authenticated by the mvv42 cookie of a fresh (or the given) Telegram user.</summary>
    public HttpClient CookieClient(long? telegramId = null, IReadOnlyList<string>? apps = null, bool isAdmin = false, string name = "Max") =>
        CreateClient().WithCookie(Mvv42TestAuth.CreateToken(telegramId ?? NewTelegramId(), name, apps, isAdmin));

    public HttpClient BearerClient(string token) => CreateClient().WithBearer(token);

    /// <summary>Links a device for a fresh cookie user; returns the cookie client, the minted response and a bearer client.</summary>
    public async Task<Linked> LinkAsync(string? deviceId = null, long? telegramId = null)
    {
        var cookie = CookieClient(telegramId);
        deviceId ??= NewDeviceId();
        var res = await cookie.PostAsJsonAsync("/api/devices", new { deviceId, name = "MAX-PC" });
        res.EnsureSuccessStatusCode();
        var minted = (await res.Content.ReadFromJsonAsync<JsonElement>());
        var token = minted.GetProperty("token").GetString()!;
        return new Linked(cookie, BearerClient(token), token, deviceId, minted.GetProperty("accountId").GetString()!);
    }

    public static string NewDeviceId() => Guid.NewGuid().ToString("D"); // v4, lowercase

    public void WriteIndex(string json) => File.WriteAllText(Path.Combine(SamplesRoot, "index.json"), json);

    public void WriteSampleFile(string lib, string path, byte[] bytes)
    {
        var full = Path.Combine(SamplesRoot, lib, path.Replace('/', Path.DirectorySeparatorChar));
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        File.WriteAllBytes(full, bytes);
    }
}

public sealed record Linked(HttpClient Cookie, HttpClient Bearer, string Token, string DeviceId, string AccountId);
