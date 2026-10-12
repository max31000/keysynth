using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using Keysynth.Core;
using Keysynth.IntegrationTests.Infrastructure;
using Xunit;

namespace Keysynth.IntegrationTests;

/// <summary>Devices (cloud-api.md §3.1).</summary>
public sealed class DeviceEndpointTests(KeysynthFactory factory) : IClassFixture<KeysynthFactory>
{
    [Fact]
    public async Task MintReturnsTokenOnceAndDeviceMeWorks()
    {
        var cookie = factory.CookieClient(name: "Макс");
        var deviceId = KeysynthFactory.NewDeviceId();
        var res = await cookie.PostAsJsonAsync("/api/devices", new { deviceId, name = "  MAX-PC  " });
        res.StatusCode.Should().Be(HttpStatusCode.Created);
        res.Headers.CacheControl!.NoStore.Should().BeTrue();

        var minted = await res.Content.ReadFromJsonAsync<JsonElement>();
        minted.GetProperty("deviceId").GetString().Should().Be(deviceId);
        var token = minted.GetProperty("token").GetString()!;
        DeviceTokens.IsWellFormed(token).Should().BeTrue();
        minted.GetProperty("expiresAt").GetString().Should().Be("2027-04-10T12:00:00Z");
        minted.GetProperty("accountId").ValueKind.Should().Be(JsonValueKind.String);
        minted.GetProperty("accountName").GetString().Should().Be("Макс");

        var me = await factory.BearerClient(token).GetFromJsonAsync<JsonElement>("/api/devices/me");
        me.GetProperty("deviceId").GetString().Should().Be(deviceId);
        me.GetProperty("name").GetString().Should().Be("MAX-PC");
        me.GetProperty("accountId").GetString().Should().Be(minted.GetProperty("accountId").GetString());
        me.GetProperty("accountName").GetString().Should().Be("Макс");
        me.GetProperty("expiresAt").GetString().Should().Be("2027-04-10T12:00:00Z");
        me.TryGetProperty("token", out _).Should().BeFalse();

        var list = await cookie.GetFromJsonAsync<JsonElement>("/api/devices");
        var item = list.GetProperty("items").EnumerateArray().Single();
        item.GetProperty("deviceId").GetString().Should().Be(deviceId);
        item.GetProperty("name").GetString().Should().Be("MAX-PC");
        item.GetProperty("createdAt").GetString().Should().Be("2026-10-12T12:00:00Z");
        item.GetProperty("lastSeenAt").ValueKind.Should().NotBe(JsonValueKind.Undefined);
        item.TryGetProperty("token", out _).Should().BeFalse();
    }

    [Fact]
    public async Task RemintReplacesTokenImmediately()
    {
        var linked = await factory.LinkAsync();
        var res = await linked.Cookie.PostAsJsonAsync("/api/devices", new { deviceId = linked.DeviceId, name = "RENAMED" });
        res.StatusCode.Should().Be(HttpStatusCode.OK);
        var newToken = (await res.Content.ReadFromJsonAsync<JsonElement>()).GetProperty("token").GetString()!;
        newToken.Should().NotBe(linked.Token);

        await (await linked.Bearer.GetAsync("/api/devices/me")).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
        var me = await factory.BearerClient(newToken).GetFromJsonAsync<JsonElement>("/api/devices/me");
        me.GetProperty("name").GetString().Should().Be("RENAMED");
        (await linked.Cookie.GetFromJsonAsync<JsonElement>("/api/devices")).GetProperty("items").GetArrayLength().Should().Be(1);
    }

    [Fact]
    public async Task SameDeviceIdForTwoAccountsIsTwoRows()
    {
        var deviceId = KeysynthFactory.NewDeviceId();
        var a = await factory.LinkAsync(deviceId);
        var b = await factory.LinkAsync(deviceId);
        a.AccountId.Should().NotBe(b.AccountId);
        (await a.Bearer.GetAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.OK);
        (await b.Bearer.GetAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task DeleteByIdRevokes()
    {
        var linked = await factory.LinkAsync();
        (await linked.Cookie.DeleteAsync($"/api/devices/{linked.DeviceId}")).StatusCode.Should().Be(HttpStatusCode.NoContent);
        await (await linked.Bearer.GetAsync("/api/devices/me")).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
        await (await linked.Cookie.DeleteAsync($"/api/devices/{linked.DeviceId}")).ShouldBe(HttpStatusCode.NotFound, "not_found");
        await (await linked.Cookie.DeleteAsync("/api/devices/not-a-uuid")).ShouldBe(HttpStatusCode.NotFound, "not_found");
    }

    [Fact]
    public async Task CannotDeleteAnotherUsersDevice()
    {
        var victim = await factory.LinkAsync();
        var attacker = factory.CookieClient();
        await (await attacker.DeleteAsync($"/api/devices/{victim.DeviceId}")).ShouldBe(HttpStatusCode.NotFound, "not_found");
        (await victim.Bearer.GetAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task DeleteMeRevokes()
    {
        var linked = await factory.LinkAsync();
        (await linked.Bearer.DeleteAsync("/api/devices/me")).StatusCode.Should().Be(HttpStatusCode.NoContent);
        await (await linked.Bearer.GetAsync("/api/devices/me")).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
        (await linked.Cookie.GetFromJsonAsync<JsonElement>("/api/devices")).GetProperty("items").GetArrayLength().Should().Be(0);
    }

    [Fact]
    public async Task TenDevicesMax()
    {
        var linked = await factory.LinkAsync();
        var userId = long.Parse(linked.AccountId);
        // seed directly: 10 POSTs would also hit the 10/hour mint limit
        for (var i = 1; i < Limits.MaxDevicesPerUser; i++)
            await factory.Store.MintAsync(new DeviceMint(userId, KeysynthFactory.NewDeviceId(), $"PC{i}",
                DeviceTokens.Hash(DeviceTokens.Generate()), factory.Clock.UtcNow, factory.Clock.UtcNow.AddDays(1)), Limits.MaxDevicesPerUser);
        await (await linked.Cookie.PostAsJsonAsync("/api/devices", new { deviceId = KeysynthFactory.NewDeviceId(), name = "PC11" }))
            .ShouldBe(HttpStatusCode.Conflict, "too_many_devices");
        // re-mint of an existing device is still allowed at the limit
        (await linked.Cookie.PostAsJsonAsync("/api/devices", new { deviceId = linked.DeviceId, name = "PC0" }))
            .StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task MintIsRateLimited10PerHour()
    {
        var cookie = factory.CookieClient();
        var deviceId = KeysynthFactory.NewDeviceId();
        for (var i = 0; i < Limits.DeviceMintPerHour; i++)
            (await cookie.PostAsJsonAsync("/api/devices", new { deviceId, name = "PC" })).IsSuccessStatusCode.Should().BeTrue();
        var res = await cookie.PostAsJsonAsync("/api/devices", new { deviceId, name = "PC" });
        await res.ShouldBe(HttpStatusCode.TooManyRequests, "rate_limited");
        int.Parse(res.Headers.GetValues("Retry-After").Single()).Should().BeInRange(1, 3600);
        // other users are unaffected
        (await factory.CookieClient().PostAsJsonAsync("/api/devices", new { deviceId, name = "PC" })).StatusCode.Should().Be(HttpStatusCode.Created);
    }

    public static TheoryData<string> InvalidMintBodies => new()
    {
        "not json",
        "[]",
        """{"name":"PC"}""",
        """{"deviceId":"3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40"}""",
        """{"deviceId":"3F0C7E9A-5B1D-4C2E-9A77-0E5D2C1B8F40","name":"PC"}""",
        """{"deviceId":"not-a-uuid","name":"PC"}""",
        """{"deviceId":"3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40","name":"   "}""",
        """{"deviceId":"3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40","name":"a\u0001b"}""",
        """{"deviceId":"3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40","name":42}""",
        "{\"deviceId\":\"3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40\",\"name\":\"" + new string('x', 101) + "\"}",
    };

    [Theory]
    [MemberData(nameof(InvalidMintBodies))]
    public async Task InvalidMintIs400(string body)
    {
        var res = await factory.CookieClient().PostAsync("/api/devices", new StringContent(body, Encoding.UTF8, "application/json"));
        await res.ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
    }

    [Fact]
    public async Task NameOf100CharsIsAccepted()
    {
        var res = await factory.CookieClient().PostAsJsonAsync("/api/devices",
            new { deviceId = KeysynthFactory.NewDeviceId(), name = new string('я', 100) });
        res.StatusCode.Should().Be(HttpStatusCode.Created);
    }
}
