using System.Net;
using System.Text.Json;
using FluentAssertions;
using Keysynth.Api.Relay;
using Keysynth.IntegrationTests.Infrastructure;
using Xunit;

namespace Keysynth.IntegrationTests;

/// <summary>Relay hub (docs/PROTOCOL.md *Relay*, spec docs/specs/2026-10-12-remote.md §10).</summary>
public sealed class RelayTests
{
    private static readonly TimeSpan Short = TimeSpan.FromMilliseconds(300);

    private static HttpRequestMessage Get(string query, string? cookie = null, string? bearer = null, string? origin = null)
    {
        var req = new HttpRequestMessage(HttpMethod.Get, "/api/relay" + query);
        if (cookie is not null)
            req.Headers.Add("Cookie", $"{Mvv42TestAuth.CookieName}={cookie}");
        if (bearer is not null)
            req.Headers.TryAddWithoutValidation("Authorization", "Bearer " + bearer);
        if (origin is not null)
            req.Headers.Add("Origin", origin);
        return req;
    }

    private static async Task<int> HandshakeStatus(RelayFactory f, string query, Action<IDictionary<string, Microsoft.Extensions.Primitives.StringValues>> headers)
    {
        try
        {
            await using var s = await RelaySocket.ConnectAsync(f, query, headers);
            return 101;
        }
        catch (InvalidOperationException e) when (e.Message.Contains("status code", StringComparison.OrdinalIgnoreCase))
        {
            return int.Parse(e.Message[(e.Message.LastIndexOf(' ') + 1)..].Trim('.'), System.Globalization.CultureInfo.InvariantCulture);
        }
    }

    // ---- handshake: auth matrix, Origin, ambiguity ----

    [Fact]
    public async Task AuthMatrixBeforeTheUpgrade()
    {
        await using var f = new RelayFactory();
        var dev = await f.LinkAsync();
        var tg = f.NewTelegramId();
        var cookie = Mvv42TestAuth.CreateToken(tg);
        var http = f.CreateClient();
        const string origin = Mvv42TestAuth.AllowedOrigin;

        // role
        await (await http.SendAsync(Get("", bearer: dev.Token))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        await (await http.SendAsync(Get("?role=admin", bearer: dev.Token))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        // host
        await (await http.SendAsync(Get("?role=host"))).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
        await (await http.SendAsync(Get("?role=host", bearer: "ksd_" + new string('x', 43)))).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
        await (await http.SendAsync(Get("?role=host", bearer: "garbage"))).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
        await (await http.SendAsync(Get("?role=host", cookie: cookie, origin: origin))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        // controller
        await (await http.SendAsync(Get("?role=controller", origin: origin))).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
        await (await http.SendAsync(Get("?role=controller", bearer: dev.Token, origin: origin))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await http.SendAsync(Get("?role=controller", cookie: Mvv42TestAuth.CreateToken(tg, apps: ["bonds"]), origin: origin)))
            .ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await http.SendAsync(Get("?role=controller", cookie: Mvv42TestAuth.CreateToken(tg, lifetime: TimeSpan.FromMinutes(-5)), origin: origin)))
            .ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
        await (await http.SendAsync(Get("?role=controller&cid=bad id", cookie: cookie, origin: origin))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        // valid credentials, but not a WebSocket request
        await (await http.SendAsync(Get("?role=host", bearer: dev.Token))).ShouldBe(HttpStatusCode.BadRequest, "websocket_required");
        await (await http.SendAsync(Get("?role=controller", cookie: cookie, origin: origin))).ShouldBe(HttpStatusCode.BadRequest, "websocket_required");

        // and the same over a real upgrade
        (await HandshakeStatus(f, "host", h => h["Authorization"] = "Bearer " + dev.Token)).Should().Be(101);
        (await HandshakeStatus(f, "host", _ => { })).Should().Be(401);
        (await HandshakeStatus(f, "controller", h => { h["Cookie"] = $"{Mvv42TestAuth.CookieName}={cookie}"; h["Origin"] = origin; })).Should().Be(101);
        (await HandshakeStatus(f, "controller", h => h["Cookie"] = $"{Mvv42TestAuth.CookieName}={cookie}")).Should().Be(403);
    }

    [Fact]
    public async Task ExpiredOrStaleDeviceTokenIs401WithItsCode()
    {
        await using var f = new RelayFactory();
        var dev = await f.LinkAsync();
        var http = f.CreateClient();
        f.Clock.Advance(TimeSpan.FromDays(31)); // owner's last_app_ok_at too old
        await (await http.SendAsync(Get("?role=host", bearer: dev.Token))).ShouldBe(HttpStatusCode.Unauthorized, "reauth_required");
    }

    [Fact]
    public async Task ControllerOriginMissingOrForeignIs403()
    {
        await using var f = new RelayFactory();
        var cookie = Mvv42TestAuth.CreateToken(f.NewTelegramId());
        var http = f.CreateClient();
        await (await http.SendAsync(Get("?role=controller", cookie: cookie))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await http.SendAsync(Get("?role=controller", cookie: cookie, origin: "https://evil.example"))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await http.SendAsync(Get("?role=controller", cookie: cookie, origin: "https://www.mvv42.ru"))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await http.SendAsync(Get("?role=controller", cookie: cookie, origin: "null"))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
    }

    [Fact]
    public async Task CookieAndBearerTogetherIs400()
    {
        await using var f = new RelayFactory();
        var dev = await f.LinkAsync();
        var cookie = Mvv42TestAuth.CreateToken(f.NewTelegramId());
        var http = f.CreateClient();
        foreach (var role in new[] { "host", "controller" })
            await (await http.SendAsync(Get("?role=" + role, cookie: cookie, bearer: dev.Token, origin: Mvv42TestAuth.AllowedOrigin)))
                .ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
    }

    // ---- rooms, routing ----

    [Fact]
    public async Task HostsListJoinedEventsAndRouting()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);

        await using var c1 = await f.ControllerAsync(tg, "controller-1");
        (await c1.ExpectAsync("hosts")).GetProperty("hosts").GetArrayLength().Should().Be(0);

        await using var host = await f.OnlineHostAsync(dev, "Пианино");
        var hosts = await c1.ExpectAsync("hosts");
        var entry = hosts.GetProperty("hosts")[0];
        entry.GetProperty("deviceId").GetString().Should().Be(dev.DeviceId);
        entry.GetProperty("name").GetString().Should().Be("Пианино");
        entry.GetProperty("version").GetString().Should().Be("0.2.0");
        entry.GetProperty("online").GetBoolean().Should().BeTrue();
        // host learns about the controller that was already there
        var joined = await host.ExpectAsync("controller");
        joined.GetProperty("cid").GetString().Should().Be("controller-1");
        joined.GetProperty("joined").GetBoolean().Should().BeTrue();

        await using var c2 = await f.ControllerAsync(tg, "controller-2");
        (await c2.ExpectAsync("hosts")).GetProperty("hosts")[0].GetProperty("online").GetBoolean().Should().BeTrue();
        (await host.ExpectAsync("controller")).GetProperty("cid").GetString().Should().Be("controller-2");

        // controller -> host
        await c1.SendAsync(new { t = "msg", host = dev.DeviceId, msg = new { type = "hello", id = 1 } });
        var fromC1 = await host.ExpectAsync("msg");
        fromC1.GetProperty("from").GetString().Should().Be("controller-1");
        fromC1.GetProperty("msg").GetProperty("type").GetString().Should().Be("hello");
        fromC1.GetProperty("msg").GetProperty("id").GetInt32().Should().Be(1);

        // host -> one controller
        await host.SendAsync(new { t = "msg", to = "controller-1", msg = new { type = "state", id = 1 } });
        var toC1 = await c1.ExpectAsync("msg");
        toC1.GetProperty("host").GetString().Should().Be(dev.DeviceId);
        toC1.GetProperty("msg").GetProperty("id").GetInt32().Should().Be(1);
        // host -> all
        await host.SendAsync(new { t = "msg", to = "*", msg = new { type = "param", v = 1 } });
        (await c1.ExpectAsync("msg")).GetProperty("msg").GetProperty("v").GetInt32().Should().Be(1);
        (await c2.ExpectAsync("msg")).GetProperty("msg").GetProperty("v").GetInt32().Should().Be(1);
        // host -> all except the origin
        await host.SendAsync(new { t = "msg", to = "*", except = "controller-1", msg = new { type = "param", v = 2 } });
        (await c2.ExpectAsync("msg")).GetProperty("msg").GetProperty("v").GetInt32().Should().Be(2);
        await host.SyncAsync();
        (await c1.QuietAsync(Short)).Should().BeTrue("excepted controller gets nothing");
        // to a cid that is gone: dropped silently
        await host.SendAsync(new { t = "msg", to = "nobody-here", msg = new { type = "x" } });
        await host.SyncAsync();

        // pause / resume are forwarded; lossy frames skip a paused controller
        await c2.SendAsync(new { t = "pause" });
        var pause = await host.ExpectAsync("pause");
        pause.GetProperty("cid").GetString().Should().Be("controller-2");
        await host.SendAsync(new { t = "msg", to = "*", lossy = true, msg = new { type = "telemetry" } });
        await host.SendAsync(new { t = "msg", to = "*", msg = new { type = "state" } });
        (await c1.ExpectAsync("msg")).GetProperty("msg").GetProperty("type").GetString().Should().Be("telemetry");
        (await c2.ExpectAsync("msg")).GetProperty("msg").GetProperty("type").GetString().Should().Be("state", "paused: lossy skipped, non-lossy kept");
        await c2.SendAsync(new { t = "resume" });
        (await host.ExpectAsync("resume")).GetProperty("cid").GetString().Should().Be("controller-2");

        // controller leaves -> host told; host leaves -> controllers see it offline
        await c2.DisposeAsync();
        var left = await host.ExpectAsync("controller", j => !j.GetProperty("joined").GetBoolean());
        left.GetProperty("cid").GetString().Should().Be("controller-2");
        await host.DisposeAsync();
        var offline = await c1.ExpectAsync("hosts");
        offline.GetProperty("hosts")[0].GetProperty("online").GetBoolean().Should().BeFalse();

        // message to an offline host -> error with the request id
        await c1.SendAsync(new { t = "msg", host = dev.DeviceId, msg = new { type = "get_patch", id = 7 } });
        var err = await c1.ExpectAsync("error");
        err.GetProperty("code").GetString().Should().Be("host_offline");
        err.GetProperty("host").GetString().Should().Be(dev.DeviceId);
        err.GetProperty("id").GetInt32().Should().Be(7);
    }

    [Fact]
    public async Task EnvelopeValidation()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.HostAsync(dev.Token);
        // msg before hello
        await host.SendAsync(new { t = "msg", to = "*", msg = new { type = "x" } });
        (await host.ExpectAsync("error")).GetProperty("code").GetString().Should().Be("hello_required");
        await host.SendAsync(new { t = "hello", host = new { deviceId = "spoofed", name = "PC", version = "1" } });
        await using var c = await f.ControllerAsync(tg);
        var hosts = await c.ExpectAsync("hosts");
        hosts.GetProperty("hosts")[0].GetProperty("deviceId").GetString().Should().Be(dev.DeviceId, "deviceId comes from the token");

        foreach (var bad in new[] { "not json", "[]", """{"t":"nope"}""", """{"t":"msg","to":"*"}""", """{"t":"msg","to":"*","msg":"x"}""",
                     """{"t":"msg","to":"*","lossy":"yes","msg":{}}""", """{"t":"msg","to":"*","except":5,"msg":{}}""" })
        {
            await host.SendRawAsync(bad);
            (await host.ExpectAsync("error")).GetProperty("code").GetString().Should().Be("bad_envelope", bad);
        }
        foreach (var bad in new[] { "{", """{"t":"msg","msg":{}}""", """{"t":"msg","host":"x","msg":[1]}""", """{"t":"hello"}""" })
        {
            await c.SendRawAsync(bad);
            (await c.ExpectAsync("error")).GetProperty("code").GetString().Should().Be("bad_envelope", bad);
        }
        await c.SendBinaryAsync([1, 2, 3]);
        (await c.ClosedAsync()).Should().Be(RelayClose.UnsupportedData);
    }

    [Fact]
    public async Task UsersNeverSeeEachOther()
    {
        await using var f = new RelayFactory();
        var tgA = f.NewTelegramId();
        var tgB = f.NewTelegramId();
        var devA = await f.LinkAsync(telegramId: tgA);
        var devB = await f.LinkAsync(telegramId: tgB);
        await using var hostA = await f.OnlineHostAsync(devA);
        await using var hostB = await f.OnlineHostAsync(devB);
        await using var ctlB = await f.ControllerAsync(tgB, "same-cid-both");
        await using var ctlA = await f.ControllerAsync(tgA, "same-cid-both");

        var listB = await ctlB.ExpectAsync("hosts");
        listB.GetProperty("hosts").EnumerateArray().Select(h => h.GetProperty("deviceId").GetString()).Should().Equal(devB.DeviceId);

        // B addresses A's device: offline for B
        await ctlB.SendAsync(new { t = "msg", host = devA.DeviceId, msg = new { type = "panic" } });
        (await ctlB.ExpectAsync("error")).GetProperty("code").GetString().Should().Be("host_offline");
        // A's broadcast reaches only A's controller (same cid in both rooms)
        await hostA.SendAsync(new { t = "msg", to = "*", msg = new { type = "state", who = "A" } });
        (await ctlA.ExpectAsync("msg")).GetProperty("msg").GetProperty("who").GetString().Should().Be("A");
        await hostA.SendAsync(new { t = "msg", to = "same-cid-both", msg = new { type = "state", who = "A2" } });
        (await ctlA.ExpectAsync("msg")).GetProperty("msg").GetProperty("who").GetString().Should().Be("A2");
        await hostA.SyncAsync();
        (await ctlB.QuietAsync(Short)).Should().BeTrue();
        // host B never heard of controller A
        (await hostB.ExpectAsync("controller")).GetProperty("cid").GetString().Should().Be("same-cid-both");
        await hostB.SyncAsync();
        (await hostB.QuietAsync(Short)).Should().BeTrue();
    }

    [Fact]
    public async Task DuplicateHostReplacesTheOldSocket()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var c = await f.ControllerAsync(tg, "ctl-dup-host");
        await c.ExpectAsync("hosts");
        await using var first = await f.OnlineHostAsync(dev);
        await c.ExpectAsync("hosts", h => h.GetProperty("hosts")[0].GetProperty("online").GetBoolean());

        await using var second = await f.HostAsync(dev.Token);
        (await first.ClosedAsync()).Should().Be(RelayClose.Replaced);
        await c.ExpectAsync("hosts", h => !h.GetProperty("hosts")[0].GetProperty("online").GetBoolean());
        await second.SendAsync(new { t = "hello", host = new { deviceId = dev.DeviceId, name = "MAX-PC", version = "0.2.1" } });
        var back = await c.ExpectAsync("hosts", h => h.GetProperty("hosts")[0].GetProperty("online").GetBoolean());
        back.GetProperty("hosts")[0].GetProperty("version").GetString().Should().Be("0.2.1");
        (await second.ExpectAsync("controller")).GetProperty("cid").GetString().Should().Be("ctl-dup-host");
    }

    [Fact]
    public async Task ConnectionCountLimits()
    {
        await using var f = new RelayFactory(o => { o.MaxHostsPerUser = 2; o.MaxControllersPerUser = 2; });
        var tg = f.NewTelegramId();
        var d1 = await f.LinkAsync(telegramId: tg);
        var d2 = await f.LinkAsync(telegramId: tg);
        var d3 = await f.LinkAsync(telegramId: tg);
        await using var h1 = await f.HostAsync(d1.Token);
        await using var h2 = await f.HostAsync(d2.Token);
        await h1.SyncAsync(); // joined (the join precedes the read loop)
        await h2.SyncAsync();
        await using var h3 = await f.HostAsync(d3.Token);
        (await h3.ClosedAsync()).Should().Be(RelayClose.TooMany);
        await using var c1 = await f.ControllerAsync(tg, "limit-ctl-1");
        await using var c2 = await f.ControllerAsync(tg, "limit-ctl-2");
        await c1.ExpectAsync("hosts");
        await c2.ExpectAsync("hosts");
        await using var c3 = await f.ControllerAsync(tg, "limit-ctl-3");
        (await c3.ClosedAsync()).Should().Be(RelayClose.TooMany);
        // a resume of a known cid is not a new controller
        await using var c1b = await f.ControllerAsync(tg, "limit-ctl-1");
        await c1b.ExpectAsync("hosts");
        (await c1.ClosedAsync()).Should().Be(RelayClose.Replaced);
        h1.IsClosed.Should().BeFalse();
    }

    [Fact]
    public async Task CidResumeKeepsTheControllerForHosts()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        await using var old = await f.ControllerAsync(tg, "resume-cid-1");
        await old.ExpectAsync("hosts");
        await host.ExpectAsync("controller", j => j.GetProperty("joined").GetBoolean());
        await old.SendAsync(new { t = "pause" });
        await host.ExpectAsync("pause");

        await using var fresh = await f.ControllerAsync(tg, "resume-cid-1");
        (await fresh.ExpectAsync("hosts")).GetProperty("hosts")[0].GetProperty("online").GetBoolean().Should().BeTrue();
        (await old.ClosedAsync()).Should().Be(RelayClose.Replaced);
        // no left/joined for the resume, only the pause state reset
        (await host.ExpectAsync("resume")).GetProperty("cid").GetString().Should().Be("resume-cid-1");
        await host.SendAsync(new { t = "msg", to = "resume-cid-1", msg = new { type = "state", id = 3 } });
        (await fresh.ExpectAsync("msg")).GetProperty("msg").GetProperty("id").GetInt32().Should().Be(3);
        await host.SyncAsync();
        (await host.QuietAsync(Short)).Should().BeTrue("no controller joined/left for a resume");
    }

    // ---- backpressure, caps ----

    [Fact]
    public async Task LossyFramesAreDroppedNonLossyOverflowCloses4008()
    {
        var gate = new SemaphoreSlim(0);
        await using var f = new RelayFactory(o =>
        {
            o.LossyQueueFrames = 4;
            o.QueueFrames = 16;
            o.BeforeSendForTests = async (conn, ct) =>
            {
                if (conn.Role == RelayRole.Controller && conn.Id.StartsWith("slow", StringComparison.Ordinal))
                    await gate.WaitAsync(ct);
            };
        });
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        await using var slow = await f.ControllerAsync(tg, "slow-ctl-1"); // writer parks before its first frame (hosts)
        await host.ExpectAsync("controller");

        for (var i = 0; i < 50; i++)
            await host.SendAsync(new { t = "msg", to = "*", lossy = true, msg = new { type = "telemetry", i } });
        await host.SyncAsync();
        gate.Release(1000);
        var received = 0;
        await slow.ExpectAsync("hosts");
        while (!await slow.QuietAsync(Short))
        {
            await slow.NextAsync();
            received++;
        }
        received.Should().BeInRange(1, 4, "only the lossy mark's worth was queued");
        slow.IsClosed.Should().BeFalse();

        // non-lossy over the hard cap: the controller is closed (it reconnects and re-hellos)
        while (gate.CurrentCount > 0)
            await gate.WaitAsync();
        await using var slow2 = await f.ControllerAsync(tg, "slow-ctl-2");
        await host.ExpectAsync("controller", j => j.GetProperty("cid").GetString() == "slow-ctl-2");
        for (var i = 0; i < 20; i++)
            await host.SendAsync(new { t = "msg", to = "slow-ctl-2", msg = new { type = "state", i } });
        await host.SyncAsync();
        gate.Release(1000);
        (await slow2.ClosedAsync()).Should().Be(RelayClose.Overflow);
        host.IsClosed.Should().BeFalse();
    }

    [Fact]
    public async Task RateCapCloses4029()
    {
        await using var f = new RelayFactory(o => o.ControllerMessagesPerSecond = 10);
        var tg = f.NewTelegramId();
        await using var c = await f.ControllerAsync(tg);
        await c.ExpectAsync("hosts");
        for (var i = 0; i < 40 && !c.IsClosed; i++)
            await c.SendAsync(new { t = "ping" });
        (await c.ClosedAsync()).Should().Be(RelayClose.RateLimited);
    }

    [Fact]
    public async Task FrameCaps()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        await using var c = await f.ControllerAsync(tg);
        await c.ExpectAsync("hosts");
        // host -> controller: ~900 KB snapshot passes
        var big = new string('a', 900 * 1024);
        await host.SendAsync(new { t = "msg", to = "*", msg = new { type = "catalog_ok", blob = big } });
        (await c.ExpectAsync("msg")).GetProperty("msg").GetProperty("blob").GetString()!.Length.Should().Be(big.Length);
        // controller over 128 KB -> 1009
        await c.SendAsync(new { t = "msg", host = dev.DeviceId, msg = new { type = "set_patch", blob = new string('b', 129 * 1024) } });
        (await c.ClosedAsync()).Should().Be(RelayClose.TooBig);
        // host over 1 MB -> 1009
        await host.SendAsync(new { t = "msg", to = "*", msg = new { type = "x", blob = new string('c', 1024 * 1024) } });
        (await host.ClosedAsync()).Should().Be(RelayClose.TooBig);
    }

    // ---- heartbeat, ages, revocation ----

    [Fact]
    public async Task HeartbeatPingsAndSilenceTimeout()
    {
        await using var f = new RelayFactory(o => { o.PingInterval = TimeSpan.FromMilliseconds(100); o.SilenceTimeout = TimeSpan.FromMilliseconds(1500); });
        var tg = f.NewTelegramId();
        await using var alive = await f.ControllerAsync(tg, "alive-ctl");   // answers pings
        await using var silent = await f.ControllerAsync(tg, "silent-ctl");
        silent.AutoPong = false;
        await silent.ExpectAsync("ping");
        (await silent.ClosedAsync()).Should().Be(RelayClose.Timeout);
        await Task.Delay(TimeSpan.FromMilliseconds(500));
        alive.IsClosed.Should().BeFalse("pongs keep it alive past the silence timeout (closed: {0})", alive.IsClosed ? await alive.ClosedAsync() : 0);
    }

    [Fact]
    public async Task ControllerMaxAgeCloses4002()
    {
        await using var f = new RelayFactory(o => o.ControllerMaxAge = TimeSpan.FromMilliseconds(300));
        await using var c = await f.ControllerAsync(f.NewTelegramId());
        (await c.ClosedAsync()).Should().Be(RelayClose.MaxAge);
    }

    [Fact]
    public async Task DeletedDeviceIsClosedAtOnceAndForgotten()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        await using var c = await f.ControllerAsync(tg);
        await c.ExpectAsync("hosts", h => h.GetProperty("hosts").GetArrayLength() == 1);
        (await dev.Cookie.DeleteAsync("/api/devices/" + dev.DeviceId)).StatusCode.Should().Be(HttpStatusCode.NoContent);
        (await host.ClosedAsync()).Should().Be(RelayClose.Revoked);
        await c.ExpectAsync("hosts", h => h.GetProperty("hosts").GetArrayLength() == 0);
        // and it cannot come back with the old token
        await (await f.CreateClient().SendAsync(Get("?role=host", bearer: dev.Token))).ShouldBe(HttpStatusCode.Unauthorized, "token_invalid");
    }

    [Fact]
    public async Task HostTokenIsRevalidatedPeriodically()
    {
        await using var f = new RelayFactory(o => o.HostRevalidateInterval = TimeSpan.FromMilliseconds(100));
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        await host.SyncAsync();
        await Task.Delay(TimeSpan.FromMilliseconds(300));
        host.IsClosed.Should().BeFalse("still valid");
        f.Clock.Advance(TimeSpan.FromDays(31)); // reauth_required
        (await host.ClosedAsync()).Should().Be(RelayClose.Revoked);
    }

    [Fact]
    public async Task DeviceDeletedBehindTheHubIsCaughtByRevalidation()
    {
        await using var f = new RelayFactory(o => o.HostRevalidateInterval = TimeSpan.FromMilliseconds(100));
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        var userId = long.Parse(dev.AccountId, System.Globalization.CultureInfo.InvariantCulture);
        await f.Store.DeleteAsync(userId, dev.DeviceId); // e.g. another API instance / DB admin
        (await host.ClosedAsync()).Should().Be(RelayClose.Revoked);
    }

    [Fact]
    public async Task ServerFramesAreValidJsonWithoutTokens()
    {
        await using var f = new RelayFactory();
        var tg = f.NewTelegramId();
        var dev = await f.LinkAsync(telegramId: tg);
        await using var host = await f.OnlineHostAsync(dev);
        await using var c = await f.ControllerAsync(tg);
        var hosts = await c.ExpectAsync("hosts", h => h.GetProperty("hosts").GetArrayLength() == 1);
        JsonSerializer.Serialize(hosts).Should().NotContain(dev.Token);
    }
}
