using System.Net;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using FluentAssertions;
using Keysynth.Core;
using Keysynth.IntegrationTests.Infrastructure;
using Xunit;

namespace Keysynth.IntegrationTests;

/// <summary>Presets (cloud-api.md §3.3).</summary>
public sealed class PresetEndpointTests(KeysynthFactory factory) : IClassFixture<KeysynthFactory>
{
    private const string Patch = """{"format":1,"meta":{"name":"Warm Rhodes ☀"},"modules":[{"type":"epiano","params":{"decay":0.5}}]}""";

    private static HttpRequestMessage Put(string slug, string body, string? ifMatch = null, string? ifNoneMatch = null)
    {
        var req = new HttpRequestMessage(HttpMethod.Put, $"/api/presets/{slug}")
        {
            Content = new StringContent(body, Encoding.UTF8, "application/json"),
        };
        if (ifMatch is not null) req.Headers.TryAddWithoutValidation("If-Match", ifMatch);
        if (ifNoneMatch is not null) req.Headers.TryAddWithoutValidation("If-None-Match", ifNoneMatch);
        return req;
    }

    private static HttpRequestMessage Delete(string slug, string? ifMatch)
    {
        var req = new HttpRequestMessage(HttpMethod.Delete, $"/api/presets/{slug}");
        if (ifMatch is not null) req.Headers.TryAddWithoutValidation("If-Match", ifMatch);
        return req;
    }

    private static string Body(string patch = Patch) => $"{{\"patch\":{patch}}}";

    [Fact]
    public async Task CreateGetUpdateDeleteReviveLifecycle()
    {
        var c = (await factory.LinkAsync()).Bearer;

        var created = await c.SendAsync(Put("warm-rhodes", Body(), ifNoneMatch: "*"));
        created.StatusCode.Should().Be(HttpStatusCode.Created);
        created.Headers.ETag!.Tag.Should().Be("\"1\"");
        var meta = await created.Content.ReadFromJsonAsync<JsonElement>();
        meta.GetProperty("slug").GetString().Should().Be("warm-rhodes");
        meta.GetProperty("revision").GetInt32().Should().Be(1);
        meta.GetProperty("deleted").GetBoolean().Should().BeFalse();
        meta.GetProperty("updatedAt").GetString().Should().Be("2026-10-12T12:00:00Z");
        meta.TryGetProperty("patch", out _).Should().BeFalse();

        var got = await c.GetAsync("/api/presets/warm-rhodes");
        got.Headers.ETag!.Tag.Should().Be("\"1\"");
        var preset = await got.Content.ReadFromJsonAsync<JsonElement>();
        preset.GetProperty("patch").GetRawText().Should().Be(Patch, "stored as received");

        var updated = await c.SendAsync(Put("warm-rhodes", Body("""{"format":1,"v":2}"""), ifMatch: "\"1\""));
        updated.StatusCode.Should().Be(HttpStatusCode.OK);
        updated.Headers.ETag!.Tag.Should().Be("\"2\"");

        var deleted = await c.SendAsync(Delete("warm-rhodes", "\"2\""));
        deleted.StatusCode.Should().Be(HttpStatusCode.OK);
        deleted.Headers.ETag!.Tag.Should().Be("\"3\"");
        (await deleted.Content.ReadFromJsonAsync<JsonElement>()).GetProperty("deleted").GetBoolean().Should().BeTrue();

        var tomb = await c.GetFromJsonAsync<JsonElement>("/api/presets/warm-rhodes");
        tomb.GetProperty("deleted").GetBoolean().Should().BeTrue();
        tomb.GetProperty("revision").GetInt32().Should().Be(3);
        tomb.GetProperty("patch").ValueKind.Should().Be(JsonValueKind.Null);

        // tombstone blocks a create...
        var recreate = await c.SendAsync(Put("warm-rhodes", Body(), ifNoneMatch: "*"));
        await recreate.ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
        // ...but an If-Match on the tombstone revives it (edit wins)
        var revived = await c.SendAsync(Put("warm-rhodes", Body(), ifMatch: "\"3\""));
        revived.StatusCode.Should().Be(HttpStatusCode.OK);
        revived.Headers.ETag!.Tag.Should().Be("\"4\"");
        (await c.GetFromJsonAsync<JsonElement>("/api/presets/warm-rhodes")).GetProperty("deleted").GetBoolean().Should().BeFalse();
    }

    [Fact]
    public async Task RevisionConflictsAre412WithCurrent()
    {
        var c = (await factory.LinkAsync()).Bearer;
        (await c.SendAsync(Put("p", Body(), ifNoneMatch: "*"))).StatusCode.Should().Be(HttpStatusCode.Created);
        (await c.SendAsync(Put("p", Body(), ifMatch: "\"1\""))).StatusCode.Should().Be(HttpStatusCode.OK);

        foreach (var req in new[]
                 {
                     Put("p", Body(), ifMatch: "\"1\""),        // stale
                     Put("p", Body(), ifMatch: "\"3\""),        // future
                     Put("p", Body(), ifMatch: "W/\"2\""),      // weak never matches
                     Put("p", Body(), ifNoneMatch: "*"),        // exists
                     Delete("p", "\"1\""),
                 })
        {
            var res = await c.SendAsync(req);
            await res.ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
            var current = JsonDocument.Parse(await res.Content.ReadAsStringAsync()).RootElement.GetProperty("current");
            current.GetProperty("revision").GetInt32().Should().Be(2);
            current.GetProperty("slug").GetString().Should().Be("p");
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
        // tag lists are not supported: never match
        await (await c.SendAsync(Put("star", Body(), ifMatch: "\"3\", \"4\""))).ShouldBe(HttpStatusCode.PreconditionFailed, "revision_mismatch");
    }

    [Fact]
    public async Task PreconditionHeaders()
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put("p", Body()))).ShouldBe((HttpStatusCode)428, "precondition_required");
        await (await c.SendAsync(Delete("p", null))).ShouldBe((HttpStatusCode)428, "precondition_required");
        await (await c.SendAsync(Put("p", Body(), ifMatch: "\"1\"", ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        await (await c.SendAsync(Put("p", Body(), ifNoneMatch: "\"1\""))).ShouldBe(HttpStatusCode.BadRequest, "invalid_request");
        await (await c.SendAsync(Delete("unknown", "\"1\""))).ShouldBe(HttpStatusCode.NotFound, "not_found");
        await (await c.GetAsync("/api/presets/unknown")).ShouldBe(HttpStatusCode.NotFound, "not_found");
    }

    [Theory]
    [InlineData("Warm")]
    [InlineData("a--b")]
    [InlineData("a_b")]
    [InlineData("x.conflict-20261012T140300Z")]
    [InlineData("%D1%8F")]
    public async Task InvalidSlugIs400(string slug)
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put(slug, Body(), ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.BadRequest, "invalid_slug");
        await (await c.GetAsync($"/api/presets/{slug}")).ShouldBe(HttpStatusCode.BadRequest, "invalid_slug");
        await (await c.SendAsync(Delete(slug, "\"1\""))).ShouldBe(HttpStatusCode.BadRequest, "invalid_slug");
    }

    [Theory]
    [InlineData("""{"patch":[]}""", "invalid_patch")]
    [InlineData("""{"patch":null}""", "invalid_patch")]
    [InlineData("""{"patch":{}}""", "invalid_patch")]
    [InlineData("""{"patch":{"format":"1"}}""", "invalid_patch")]
    [InlineData("""{"other":{}}""", "invalid_patch")]
    [InlineData("""[]""", "invalid_request")]
    [InlineData("""{"patch":""", "invalid_request")]
    [InlineData("""not json""", "invalid_request")]
    public async Task InvalidBodies(string body, string code)
    {
        var c = (await factory.LinkAsync()).Bearer;
        await (await c.SendAsync(Put("p", body, ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.BadRequest, code);
    }

    [Fact]
    public async Task SizeLimits()
    {
        var c = (await factory.LinkAsync()).Bearer;
        string PatchOfSize(int bytes)
        {
            const string head = """{"format":1,"pad":""";
            const string tail = "\"}";
            return head + "\"" + new string('x', bytes - head.Length - 1 - tail.Length) + tail;
        }

        var exact = PatchOfSize(Limits.MaxPatchBytes);
        Encoding.UTF8.GetByteCount(exact).Should().Be(Limits.MaxPatchBytes);
        (await c.SendAsync(Put("at-limit", Body(exact), ifNoneMatch: "*"))).StatusCode.Should().Be(HttpStatusCode.Created);
        await (await c.SendAsync(Put("over-limit", Body(PatchOfSize(Limits.MaxPatchBytes + 1)), ifNoneMatch: "*")))
            .ShouldBe(HttpStatusCode.RequestEntityTooLarge, "payload_too_large");
        await (await c.SendAsync(Put("huge", Body(PatchOfSize(Limits.MaxRequestBodyBytes + 10)), ifNoneMatch: "*")))
            .ShouldBe(HttpStatusCode.RequestEntityTooLarge, "payload_too_large");
        (await c.GetAsync("/api/presets/over-limit")).StatusCode.Should().Be(HttpStatusCode.NotFound);
    }

    [Fact]
    public async Task ThousandPresetsMaxIncludingTombstones()
    {
        var linked = await factory.LinkAsync();
        factory.Store.Presets.SeedTombstones(long.Parse(linked.AccountId), Limits.MaxPresetsPerUser, factory.Clock.UtcNow);
        await (await linked.Bearer.SendAsync(Put("one-more", Body(), ifNoneMatch: "*"))).ShouldBe(HttpStatusCode.Conflict, "too_many_presets");
        // updating an existing row is still fine
        (await linked.Bearer.SendAsync(Put("seed-0", Body(), ifMatch: "\"2\""))).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task ListIncludesTombstonesOrderedBySlug()
    {
        var c = (await factory.LinkAsync()).Bearer;
        foreach (var slug in new[] { "b", "a", "c-1", "c" })
            (await c.SendAsync(Put(slug, Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        (await c.SendAsync(Delete("b", "\"1\""))).EnsureSuccessStatusCode();

        var items = (await c.GetFromJsonAsync<JsonElement>("/api/presets")).GetProperty("items").EnumerateArray().ToList();
        items.Select(i => i.GetProperty("slug").GetString()).Should().Equal("a", "b", "c", "c-1");
        items.Single(i => i.GetProperty("slug").GetString() == "b").GetProperty("deleted").GetBoolean().Should().BeTrue();
        items.Should().AllSatisfy(i => i.TryGetProperty("patch", out _).Should().BeFalse());
    }

    [Fact]
    public async Task PresetsArePerUser()
    {
        var a = (await factory.LinkAsync()).Bearer;
        var b = (await factory.LinkAsync()).Bearer;
        (await a.SendAsync(Put("mine", Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        (await b.GetAsync("/api/presets/mine")).StatusCode.Should().Be(HttpStatusCode.NotFound);
        (await b.GetFromJsonAsync<JsonElement>("/api/presets")).GetProperty("items").GetArrayLength().Should().Be(0);
        (await b.SendAsync(Put("mine", Body(), ifNoneMatch: "*"))).StatusCode.Should().Be(HttpStatusCode.Created);
    }

    [Fact]
    public async Task WritesRateLimited120PerMinute()
    {
        var c = (await factory.LinkAsync()).Bearer;
        (await c.SendAsync(Put("r", Body(), ifNoneMatch: "*"))).EnsureSuccessStatusCode();
        for (uint rev = 1; rev < Limits.PresetWritesPerMinute; rev++)
            (await c.SendAsync(Put("r", Body(), ifMatch: $"\"{rev}\""))).EnsureSuccessStatusCode();
        var res = await c.SendAsync(Put("r", Body(), ifMatch: $"\"{Limits.PresetWritesPerMinute}\""));
        await res.ShouldBe(HttpStatusCode.TooManyRequests, "rate_limited");
        res.Headers.Contains("Retry-After").Should().BeTrue();
        (await c.GetAsync("/api/presets")).StatusCode.Should().Be(HttpStatusCode.OK, "reads are not limited");
    }
}
