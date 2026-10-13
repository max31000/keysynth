using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using FluentAssertions;
using Keysynth.Core;
using Keysynth.IntegrationTests.Infrastructure;
using Xunit;

namespace Keysynth.IntegrationTests;

public static class Http
{
    public static async Task<string?> ErrorCode(this HttpResponseMessage res)
    {
        await res.Content.LoadIntoBufferAsync();
        var body = JsonDocument.Parse(await res.Content.ReadAsStringAsync()).RootElement;
        return body.TryGetProperty("error", out var e) ? e.GetString() : null;
    }

    public static async Task ShouldBe(this HttpResponseMessage res, HttpStatusCode status, string? error = null)
    {
        await res.Content.LoadIntoBufferAsync();
        res.StatusCode.Should().Be(status, await res.Content.ReadAsStringAsync());
        if (error is not null)
            (await res.ErrorCode()).Should().Be(error);
    }
}

/// <summary>Schemes, policies, error shapes (cloud-api.md В§1, В§2).</summary>
public sealed class AuthTests(KeysynthFactory factory) : IClassFixture<KeysynthFactory>
{
    private static readonly string[] CookieEndpoints = ["/api/devices"];
    private static readonly string[] DeviceEndpoints = ["/api/devices/me", "/api/presets", "/api/presets/x", "/api/songs", "/api/songs/x"];
    private static readonly string[] AnyUserEndpoints = ["/api/samples/manifest"];

    [Fact]
    public async Task HealthIsAnonymous()
    {
        var res = await factory.CreateClient().GetAsync("/health");
        res.StatusCode.Should().Be(HttpStatusCode.OK);
        (await res.Content.ReadFromJsonAsync<JsonElement>()).GetProperty("status").GetString().Should().Be("ok");
    }

    [Fact]
    public async Task NoCredentialsIs401Unauthorized()
    {
        var client = factory.CreateClient();
        foreach (var url in CookieEndpoints.Concat(DeviceEndpoints).Concat(AnyUserEndpoints).Append("/api/samples/files/a/b.wav"))
            await (await client.GetAsync(url)).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
        await (await client.PostAsJsonAsync("/api/devices", new { })).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
        await (await client.GetAsync("/api/unknown")).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
    }

    [Fact]
    public async Task CookieWithoutGrantIs403()
    {
        var client = factory.CookieClient(apps: ["bonds"]);
        await (await client.GetAsync("/api/devices")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await client.GetAsync("/api/samples/manifest")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
    }

    [Fact]
    public async Task AdminPassesWithoutExplicitGrant()
    {
        var client = factory.CookieClient(apps: [], isAdmin: true);
        (await client.GetAsync("/api/devices")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task CookieOnDeviceEndpointsIs403()
    {
        var client = factory.CookieClient();
        foreach (var url in DeviceEndpoints)
            await (await client.GetAsync(url)).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
    }

    [Fact]
    public async Task BearerOnCookieEndpointsIs403()
    {
        var linked = await factory.LinkAsync();
        await (await linked.Bearer.GetAsync("/api/devices")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await linked.Bearer.PostAsJsonAsync("/api/devices", new { deviceId = KeysynthFactory.NewDeviceId(), name = "X" }))
            .ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await linked.Bearer.DeleteAsync($"/api/devices/{linked.DeviceId}")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        // still linked
        (await linked.Bearer.GetAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Theory]
    [InlineData("garbage")]
    [InlineData("ksd_short")]
    [InlineData("ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")] // well-formed, unknown
    public async Task BadBearerIs401TokenInvalid(string token)
    {
        var res = await factory.BearerClient(token).GetAsync("/api/devices/me");
        await res.ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
        res.Headers.WwwAuthenticate.ToString().Should().Be("Bearer error=\"token_invalid\"");
    }

    [Fact]
    public async Task BadBearerWinsOverValidCookieOnDeviceEndpoint()
    {
        var client = factory.CookieClient().WithBearer("ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        await (await client.GetAsync("/api/presets")).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
    }

    [Fact]
    public async Task CookieOfAPlusBearerOfBCannotDriveCookieEndpoints()
    {
        var victim = await factory.LinkAsync();
        var attacker = factory.CookieClient().WithBearer(victim.Token);
        await (await attacker.GetAsync("/api/devices")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await attacker.PostAsJsonAsync("/api/devices", new { deviceId = KeysynthFactory.NewDeviceId(), name = "X" }))
            .ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await attacker.DeleteAsync($"/api/devices/{victim.DeviceId}")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        var list = await victim.Cookie.GetFromJsonAsync<JsonElement>("/api/devices");
        list.GetProperty("items").GetArrayLength().Should().Be(1);
        (await victim.Bearer.GetAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task UnauthorizedFileRequestsCarryNoXAccel()
    {
        foreach (var client in new[] { factory.CreateClient(), factory.CookieClient(apps: []), factory.BearerClient("ksd_bad") })
        {
            var res = await client.GetAsync("/api/samples/files/salamander/Sal/48khz24bit/A0v1.flac");
            ((int)res.StatusCode).Should().BeOneOf(401, 403);
            res.Headers.Contains("X-Accel-Redirect").Should().BeFalse();
        }
    }

    [Fact]
    public async Task HealthIgnoresBearer() =>
        (await factory.BearerClient("ksd_bad").GetAsync("/health")).StatusCode.Should().Be(HttpStatusCode.OK);

    [Fact]
    public async Task CookieWriteWithForeignOriginIsRejectedBySdk()
    {
        var client = factory.CookieClient();
        client.DefaultRequestHeaders.Remove("Origin");
        client.DefaultRequestHeaders.Add("Origin", "https://evil.example");
        var res = await client.PostAsJsonAsync("/api/devices", new { deviceId = KeysynthFactory.NewDeviceId(), name = "X" });
        await res.ShouldBe(HttpStatusCode.Forbidden, "origin_not_allowed");
    }

    [Fact]
    public async Task ExpiredCookieIs401()
    {
        var client = factory.CreateClient().WithCookie(Mvv42TestAuth.CreateToken(factory.NewTelegramId(), lifetime: TimeSpan.FromMinutes(-5)));
        await (await client.GetAsync("/api/devices")).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
    }

    [Fact]
    public async Task CookieRequestProvisionsUserByTelegramId()
    {
        var tg = factory.NewTelegramId();
        var a = await factory.LinkAsync(telegramId: tg);
        var b = await factory.LinkAsync(telegramId: tg);
        b.AccountId.Should().Be(a.AccountId);
        var other = await factory.LinkAsync();
        other.AccountId.Should().NotBe(a.AccountId);
        (await factory.Store.GetAsync(long.Parse(a.AccountId)))!.TelegramId.Should().Be(tg);
    }
}

/// <summary>Device token lifetime and the last_app_ok_at rule (cloud-api.md В§2.2, В§2.3). Own factory per test: time travel.</summary>
public sealed class DeviceTokenLifecycleTests
{
    [Fact]
    public async Task ExpiresAfter180Days()
    {
        await using var f = new KeysynthFactory();
        var linked = await f.LinkAsync();

        f.Clock.Advance(Limits.DeviceTokenLifetime - TimeSpan.FromSeconds(1));
        // keep last_app_ok_at fresh so only expiry is in play
        (await linked.Cookie.GetAsync("/api/devices")).EnsureSuccessStatusCode();
        (await linked.Bearer.GetAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.OK);

        f.Clock.Advance(TimeSpan.FromSeconds(1));
        var res = await linked.Bearer.GetAsync("/api/devices/me");
        await res.ShouldBe(HttpStatusCode.Unauthorized, "token_expired");
        res.Headers.WwwAuthenticate.ToString().Should().Be("Bearer error=\"token_expired\"");
    }

    [Fact]
    public async Task ReauthRequiredAfter30DaysWithoutCookieRequest_RefreshedByCookieRequest()
    {
        await using var f = new KeysynthFactory();
        var linked = await f.LinkAsync();

        f.Clock.Advance(Limits.AppOkMaxAge);
        (await linked.Bearer.GetAsync("/api/presets")).StatusCode.Should().Be(HttpStatusCode.OK, "exactly 30 days is still ok");

        f.Clock.Advance(TimeSpan.FromMilliseconds(1));
        await (await linked.Bearer.GetAsync("/api/presets")).ShouldBe(HttpStatusCode.Unauthorized, "reauth_required");

        // Opening the page (any granted cookie request) refreshes last_app_ok_at.
        (await linked.Cookie.GetAsync("/api/devices")).EnsureSuccessStatusCode();
        (await linked.Bearer.GetAsync("/api/presets")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task RevokedGrantStopsRefreshing()
    {
        await using var f = new KeysynthFactory();
        var tg = f.NewTelegramId();
        var linked = await f.LinkAsync(telegramId: tg);

        f.Clock.Advance(TimeSpan.FromDays(20));
        // grant revoked: cookie requests no longer pass mvv42:app в†’ no refresh
        await (await f.CookieClient(tg, apps: []).GetAsync("/api/devices")).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        f.Clock.Advance(TimeSpan.FromDays(11));
        await (await linked.Bearer.GetAsync("/api/devices/me")).ShouldBe(HttpStatusCode.Unauthorized, "reauth_required");
    }

    [Fact]
    public async Task LastAppOkWrittenAtMostHourly()
    {
        await using var f = new KeysynthFactory();
        var linked = await f.LinkAsync();
        var userId = long.Parse(linked.AccountId);
        var first = (await f.Store.GetAsync(userId))!.LastAppOkAt;

        f.Clock.Advance(TimeSpan.FromMinutes(59));
        (await linked.Cookie.GetAsync("/api/devices")).EnsureSuccessStatusCode();
        (await f.Store.GetAsync(userId))!.LastAppOkAt.Should().Be(first);

        f.Clock.Advance(TimeSpan.FromMinutes(1));
        (await linked.Cookie.GetAsync("/api/devices")).EnsureSuccessStatusCode();
        (await f.Store.GetAsync(userId))!.LastAppOkAt.Should().Be(f.Clock.UtcNow);
    }

    [Fact]
    public async Task LastSeenWrittenAtMostPerMinute()
    {
        await using var f = new KeysynthFactory();
        var linked = await f.LinkAsync();

        for (var i = 0; i < 3; i++)
            (await linked.Bearer.GetAsync("/api/devices/me")).EnsureSuccessStatusCode();
        f.Store.TouchCount.Should().Be(1);

        f.Clock.Advance(TimeSpan.FromMinutes(1));
        (await linked.Bearer.GetAsync("/api/devices/me")).EnsureSuccessStatusCode();
        f.Store.TouchCount.Should().Be(2);

        var list = await linked.Cookie.GetFromJsonAsync<JsonElement>("/api/devices");
        list.GetProperty("items")[0].GetProperty("lastSeenAt").GetDateTime().Should().Be(f.Clock.UtcNow);
    }
}
