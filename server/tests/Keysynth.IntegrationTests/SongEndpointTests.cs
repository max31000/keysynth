using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using Keysynth.Core;
using Keysynth.IntegrationTests.Infrastructure;
using Xunit;

namespace Keysynth.IntegrationTests;

/// <summary>Songs / setlists (cloud-api.md §3.3a): the preset scheme (§3.3) under <c>/api/songs</c>, body key <c>song</c>.</summary>
public sealed class SongEndpointTests(KeysynthFactory factory) : IClassFixture<KeysynthFactory>
{
    private const string Song = """{"format":1,"title":"Riders on the Storm ☔","items":[{"preset":"warm-rhodes","pattern":null}]}""";

    private static HttpRequestMessage Put(string slug, string body, string? ifMatch = null, string? ifNoneMatch = null, string route = "songs")
    {
        var req = new HttpRequestMessage(HttpMethod.Put, $"/api/{route}/{slug}")
        {
            Content = new StringContent(body, Encoding.UTF8, "application/json"),
        };
        if (ifMatch is not null) req.Headers.TryAddWithoutValidation("If-Match", ifMatch);
        if (ifNoneMatch is not null) req.Headers.TryAddWithoutValidation("If-None-Match", ifNoneMatch);
        return req;
    }

    private static HttpRequestMessage Delete(string slug, string? ifMatch)
    {
        var req = new HttpRequestMessage(HttpMethod.Delete, $"/api/songs/{slug}");
        if (ifMatch is not null) req.Headers.TryAddWithoutValidation("If-Match", ifMatch);
        return req;
    }

    private static string Body(string song = Song) => $"{{\"song\":{song}}}";

    [Fact]
    public async Task CreateGetUpdateDeleteReviveLifecycle()
    {
        var c = (await factory.LinkAsync()).Bearer;

        var created = await c.SendAsync(Put("riders-on-the-storm", Body(), ifNoneMatch: "*"));
        created.StatusCode.Should().Be(HttpStatusCode.Created);
        created.Headers.ETag!.Tag.Should().Be("\"1\"");
        var meta = await created.Content.ReadFromJsonAsync<JsonElement>();
        meta.GetProperty("slug").GetString().Should().Be("riders-on-the-storm");
        meta.GetProperty("revision").GetInt32().Should().Be(1);
        meta.GetProperty("deleted").GetBoolean().Should().BeFalse();
        meta.GetProperty("updatedAt").GetString().Should().Be("2026-10-12T12:00:00Z");
        meta.TryGetProperty("song", out _).Should().BeFalse();

        var got = await c.GetAsync("/api/songs/riders-on-the-storm");
        got.Headers.ETag!.Tag.Should().Be("\"1\"");
        var song = await got.Content.ReadFromJsonAsync<JsonElement>();
        song.GetProperty("song").GetRawText().Should().Be(Song, "stored as received");
        song.TryGetProperty("patch", out _).Should().BeFalse();

        var updated = await c.SendAsync(Put("riders-on-the-storm", Body("""{"format":1,"v":2}"""), ifMatch: "\"1\""));
        updated.StatusCode.Should().Be(HttpStatusCode.OK);
        updated.Headers.ETag!.Tag.Should().Be("\"2\"");
        (await c.GetFromJsonAsync<JsonElement>("/api/songs/riders-on-the-storm")).GetProperty("song").GetRawText()
            .Should().Be("""{"format":1,"v":2}""");

        var deleted = await c.SendAsync(Delete("riders-on-the-storm", "\"2\""));
        deleted.StatusCode.Should().Be(HttpStatusCode.OK);
        deleted.Headers.ETag!.Tag.Should().Be("\"3\"");
        (await deleted.Content.ReadFromJsonAsync<JsonElement>()).GetProperty("deleted").GetBoolean().Should().BeTrue();

        var tomb = await c.GetFromJsonAsync<JsonElement>("/api/songs/riders-on-the-storm");
        tomb.GetProperty("deleted").GetBoolean().Should().BeTrue();
        tomb.GetProperty("revision").GetInt32().Should().Be(3);
        tomb.GetProperty("song").ValueKind.Should().Be(JsonValueKind.Null);

        // tombstone blocks a create...
        await (await c.SendAsync(Put("riders-on-the-storm", Body(), ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
        // ...but an If-Match on the tombstone revives it
        var revived = await c.SendAsync(Put("riders-on-the-storm", Body(), ifMatch: "\"3\""));
        revived.StatusCode.Should().Be(HttpStatusCode.OK);
        revived.Headers.ETag!.Tag.Should().Be("\"4\"");
        (await c.GetFromJsonAsync<JsonElement>("/api/songs/riders-on-the-storm")).GetProperty("deleted").GetBoolean().Should().BeFalse();
    }

    [Fact]
    public async Task RevisionConflictsAre412WithCurrent()
    {
        var c = (await factory.LinkAsync()).Bearer;
        (await c.SendAsync(Put("s", Body(), ifNoneMatch: "*"))).StatusCode.Should().Be(HttpStatusCode.Created);
        (await c.SendAsync(Put("s", Body(), ifMatch: "\"1\""))).StatusCode.Should().Be(HttpStatusCode.OK);

        foreach (var req in new[]
                 {
                     Put("s", Body(), ifMatch: "\"1\""),        // stale
                     Put("s", Body(), ifMatch: "\"3\""),        // future
                     Put("s", Body(), ifMatch: "W/\"2\""),      // weak never matches
                     Put("s", Body(), ifNoneMatch: "*"),        // exists
                     Delete("s", "\"1\""),
                 })
        {
            var res = await c.SendAsync(req);
            await res.ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
            var current = JsonDocument.Parse(await res.Content.ReadAsStringAsync()).RootElement.GetProperty("current");
            current.GetProperty("revision").GetInt32().Should().Be(2);
            current.GetProperty("slug").GetString().Should().Be("s");
            current.GetProperty("deleted").GetBoolean().Should().BeFalse();
            current.TryGetProperty("song", out _).Should().BeFalse();
        }

        var missing = await c.SendAsync(Put("never-created", Body(), ifMatch: "\"1\""));
        await missing.ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
        JsonDocument.Parse(await missing.Content.ReadAsStringAsync()).RootElement.GetProperty("current").ValueKind.Should().Be(JsonValueKind.Null);
    }

    [Fact]
    public async Task IfMatchStarMatchesAnyExistingRow()
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put("star", Body(), ifMatch: "*"))).ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
        (await c.SendAsync(Put("star", Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        var upd = await c.SendAsync(Put("star", Body(), ifMatch: "*"));
        upd.StatusCode.Should().Be(HttpStatusCode.OK);
        upd.Headers.ETag!.Tag.Should().Be("\"2\"");
        (await c.SendAsync(Delete("star", "*"))).StatusCode.Should().Be(HttpStatusCode.OK);
        await (await c.SendAsync(Delete("never", "*"))).ShouldBe(HttpStatusCode.NotFound, "not_found");
    }

    [Fact]
    public async Task PreconditionHeaders()
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put("s", Body()))).ShouldBe((HttpStatusCode)428, "precondition_required");
        await (await c.SendAsync(Delete("s", null))).ShouldBe((HttpStatusCode)428, "precondition_required");
        await (await c.SendAsync(Put("s", Body(), ifMatch: "\"1\"", ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        await (await c.SendAsync(Put("s", Body(), ifNoneMatch: "\"1\""))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        await (await c.SendAsync(Delete("unknown", "\"1\""))).ShouldBe(HttpStatusCode.NotFound, "not_found");
        await (await c.GetAsync("/api/songs/unknown")).ShouldBe(HttpStatusCode.NotFound, "not_found");
    }

    [Theory]
    [InlineData("Riders")]
    [InlineData("a--b")]
    [InlineData("a_b")]
    [InlineData("x.conflict-20261012T140300Z")]
    [InlineData("%D1%8F")]
    public async Task InvalidSlugIs400(string slug)
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put(slug, Body(), ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.BadRequest, "invalid_slug");
        await (await c.GetAsync($"/api/songs/{slug}")).ShouldBe(HttpStatusCode.BadRequest, "invalid_slug");
        await (await c.SendAsync(Delete(slug, "\"1\""))).ShouldBe(HttpStatusCode.BadRequest, "invalid_slug");
    }

    [Theory]
    [InlineData("""{"song":[]}""", "invalid_song")]
    [InlineData("""{"song":null}""", "invalid_song")]
    [InlineData("""{"song":{}}""", "invalid_song")]
    [InlineData("""{"song":{"format":"1"}}""", "invalid_song")]
    [InlineData("""{"patch":{"format":1}}""", "invalid_song")]
    [InlineData("""[]""", "invalid_request")]
    [InlineData("""{"song":""", "invalid_request")]
    [InlineData("""not json""", "invalid_request")]
    public async Task InvalidBodies(string body, string code)
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put("s", body, ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.BadRequest, code);
    }

    [Fact]
    public async Task SizeLimits()
    {
        var c = (await factory.LinkAsync()).Bearer;
        string SongOfSize(int bytes)
        {
            const string head = """{"format":1,"pad":""";
            const string tail = "\"}";
            return head + "\"" + new string('x', bytes - head.Length - 1 - tail.Length) + tail;
        }

        var exact = SongOfSize(Limits.MaxSongBytes);
        Encoding.UTF8.GetByteCount(exact).Should().Be(256 * 1024);
        (await c.SendAsync(Put("at-limit", Body(exact), ifNoneMatch: "*"))).StatusCode.Should().Be(HttpStatusCode.Created);
        await (await c.SendAsync(Put("over-limit", Body(SongOfSize(Limits.MaxSongBytes + 1)), ifNoneMatch: "*")))
            .ShouldBe(HttpStatusCode.RequestEntityTooLarge, "payload_too_large");
        await (await c.SendAsync(Put("huge", Body(SongOfSize(Limits.MaxRequestBodyBytes + 10)), ifNoneMatch: "*")))
            .ShouldBe(HttpStatusCode.RequestEntityTooLarge, "payload_too_large");
        (await c.GetAsync("/api/songs/over-limit")).StatusCode.Should().Be(HttpStatusCode.NotFound);
    }

    [Fact]
    public async Task ThousandSongsMaxIncludingTombstones()
    {
        var linked = await factory.LinkAsync();
        var userId = long.Parse(linked.AccountId);
        factory.Store.Songs.SeedTombstones(userId, Limits.MaxSongsPerUser, factory.Clock.UtcNow);
        await (await linked.Bearer.SendAsync(Put("one-more", Body(), ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.Conflict, "too_many_songs");
        // updating an existing row is still fine
        (await linked.Bearer.SendAsync(Put("seed-0", Body(), ifMatch: "\"2\""))).StatusCode.Should().Be(HttpStatusCode.OK);
        // presets are counted separately
        (await linked.Bearer.SendAsync(Put("one-more", """{"patch":{"format":1}}""", ifNoneMatch: "*", route: "presets")))
            .StatusCode.Should().Be(HttpStatusCode.Created);
    }

    [Fact]
    public async Task ListIncludesTombstonesOrderedBySlug()
    {
        var c = (await factory.LinkAsync()).Bearer;
        (await c.GetFromJsonAsync<JsonElement>("/api/songs")).GetProperty("items").GetArrayLength().Should().Be(0);
        foreach (var slug in new[] { "b", "a", "c-1", "c" })
            (await c.SendAsync(Put(slug, Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        (await c.SendAsync(Delete("b", "\"1\""))).EnsureSuccessStatusCode();

        var items = (await c.GetFromJsonAsync<JsonElement>("/api/songs")).GetProperty("items").EnumerateArray().ToList();
        items.Select(i => i.GetProperty("slug").GetString()).Should().Equal("a", "b", "c", "c-1");
        var tomb = items.Single(i => i.GetProperty("slug").GetString() == "b");
        tomb.GetProperty("deleted").GetBoolean().Should().BeTrue();
        tomb.GetProperty("revision").GetInt32().Should().Be(2);
        items.Should().AllSatisfy(i => i.TryGetProperty("song", out _).Should().BeFalse());
    }

    [Fact]
    public async Task SongsAndPresetsAreSeparateResources()
    {
        var c = (await factory.LinkAsync()).Bearer;
        (await c.SendAsync(Put("same", Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        (await c.GetAsync("/api/presets/same")).StatusCode.Should().Be(HttpStatusCode.NotFound);
        (await c.GetFromJsonAsync<JsonElement>("/api/presets")).GetProperty("items").GetArrayLength().Should().Be(0);
    }

    [Fact]
    public async Task SongsArePerUser()
    {
        var a = (await factory.LinkAsync()).Bearer;
        var b = (await factory.LinkAsync()).Bearer;
        (await a.SendAsync(Put("mine", Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        (await b.GetAsync("/api/songs/mine")).StatusCode.Should().Be(HttpStatusCode.NotFound);
        (await b.GetFromJsonAsync<JsonElement>("/api/songs")).GetProperty("items").GetArrayLength().Should().Be(0);
        await (await b.SendAsync(Delete("mine", "\"1\""))).ShouldBe(HttpStatusCode.NotFound, "not_found");
        (await b.SendAsync(Put("mine", Body(), ifNoneMatch: "*"))).StatusCode.Should().Be(HttpStatusCode.Created);
        (await a.GetFromJsonAsync<JsonElement>("/api/songs/mine")).GetProperty("song").GetRawText().Should().Be(Song);
    }

    [Fact]
    public async Task CookieAuthIsRejected()
    {
        var linked = await factory.LinkAsync();
        foreach (var url in new[] { "/api/songs", "/api/songs/x" })
            await (await linked.Cookie.GetAsync(url)).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await linked.Cookie.SendAsync(Put("x", Body(), ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await linked.Cookie.SendAsync(Delete("x", "\"1\""))).ShouldBe(HttpStatusCode.Forbidden, "forbidden");
        await (await factory.CreateClient().GetAsync("/api/songs")).ShouldBe(HttpStatusCode.Unauthorized, "unauthorized");
        (await linked.Bearer.GetAsync("/api/songs/x")).StatusCode.Should().Be(HttpStatusCode.NotFound, "nothing was written");
    }

    [Fact]
    public async Task WritesShareThePresetRateLimitBucket()
    {
        var c = (await factory.LinkAsync()).Bearer;
        const int presetWrites = Limits.PresetWritesPerMinute / 2;
        (await c.SendAsync(Put("r", """{"patch":{"format":1}}""", ifNoneMatch: "*", route: "presets"))).EnsureSuccessStatusCode();
        for (uint rev = 1; rev < presetWrites; rev++)
            (await c.SendAsync(Put("r", """{"patch":{"format":1}}""", ifMatch: $"\"{rev}\"", route: "presets"))).EnsureSuccessStatusCode();

        (await c.SendAsync(Put("r", Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        for (uint rev = 1; rev < Limits.PresetWritesPerMinute - presetWrites; rev++)
            (await c.SendAsync(Put("r", Body(), ifMatch: $"\"{rev}\""))).EnsureSuccessStatusCode();

        var res = await c.SendAsync(Delete("r", $"\"{Limits.PresetWritesPerMinute - presetWrites}\""));
        await res.ShouldBe(HttpStatusCode.TooManyRequests, "rate_limited");
        res.Headers.Contains("Retry-After").Should().BeTrue();
        await (await c.SendAsync(Put("r", """{"patch":{"format":1}}""", ifMatch: $"\"{presetWrites}\"", route: "presets")))
            .ShouldBe(HttpStatusCode.TooManyRequests, "rate_limited");
        (await c.GetAsync("/api/songs")).StatusCode.Should().Be(HttpStatusCode.OK, "reads are not limited");
    }
}
