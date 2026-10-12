using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using FluentAssertions;
using Keysynth.IntegrationTests.Infrastructure;
using Xunit;

namespace Keysynth.IntegrationTests;

internal static class SampleData
{
    public const string Index = """
        {
          "format": 1,
          "generatedAt": "2026-10-12T12:00:00Z",
          "groups": [ { "id": "piano", "name": "Piano" } ],
          "libraries": [
            { "id": "salamander", "name": "Salamander", "group": "piano", "bytes": 10,
              "files": [
                { "path": "Sal/48khz24bit/A0v1.flac", "bytes": 4, "sha256": "00" },
                { "path": "Sal/Piano Samples/C4 soft.wav", "bytes": 1, "sha256": "00" },
                { "path": "Сэмплы/до #1.wav", "bytes": 1, "sha256": "00" },
                { "path": "SFZ+FLAC/100% loud?.wav", "bytes": 1, "sha256": "00" }
              ],
              "entries": [] }
          ]
        }
        """;

    public static string ETag(string json) =>
        "\"" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(json)))[..16].ToLowerInvariant() + "\"";

    /// <summary>Request path the engine sends: every segment percent-encoded.</summary>
    public static string FileUrl(string lib, string path) =>
        "/api/samples/files/" + Uri.EscapeDataString(lib) + "/" + string.Join('/', path.Split('/').Select(Uri.EscapeDataString));
}

/// <summary>Samples (cloud-api.md §3.2): manifest + X-Accel-Redirect authorisation.</summary>
public sealed class SampleEndpointTests : IClassFixture<KeysynthFactory>
{
    private readonly KeysynthFactory _f;

    public SampleEndpointTests(KeysynthFactory factory)
    {
        _f = factory;
        _f.WriteIndex(SampleData.Index);
    }

    [Fact]
    public async Task ManifestIsIndexVerbatimWithETag()
    {
        var res = await _f.CookieClient().GetAsync("/api/samples/manifest");
        res.StatusCode.Should().Be(HttpStatusCode.OK);
        res.Content.Headers.ContentType!.MediaType.Should().Be("application/json");
        res.Headers.ETag!.Tag.Should().Be(SampleData.ETag(SampleData.Index));
        res.Headers.CacheControl!.NoCache.Should().BeTrue();
        (await res.Content.ReadAsByteArrayAsync()).Should().Equal(Encoding.UTF8.GetBytes(SampleData.Index));
    }

    [Fact]
    public async Task ManifestRevalidates304ForDeviceToo()
    {
        var linked = await _f.LinkAsync();
        var req = new HttpRequestMessage(HttpMethod.Get, "/api/samples/manifest");
        req.Headers.TryAddWithoutValidation("If-None-Match", SampleData.ETag(SampleData.Index));
        var res = await linked.Bearer.SendAsync(req);
        res.StatusCode.Should().Be(HttpStatusCode.NotModified);
        res.Headers.ETag!.Tag.Should().Be(SampleData.ETag(SampleData.Index));
        (await res.Content.ReadAsByteArrayAsync()).Should().BeEmpty();

        var stale = new HttpRequestMessage(HttpMethod.Get, "/api/samples/manifest");
        stale.Headers.TryAddWithoutValidation("If-None-Match", "\"0000000000000000\"");
        (await linked.Bearer.SendAsync(stale)).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Theory]
    [InlineData("Sal/48khz24bit/A0v1.flac", "/keysynth/_files/salamander/Sal/48khz24bit/A0v1.flac")]
    [InlineData("Sal/Piano Samples/C4 soft.wav", "/keysynth/_files/salamander/Sal/Piano%20Samples/C4%20soft.wav")]
    [InlineData("Сэмплы/до #1.wav", "/keysynth/_files/salamander/%D0%A1%D1%8D%D0%BC%D0%BF%D0%BB%D1%8B/%D0%B4%D0%BE%20%231.wav")]
    [InlineData("SFZ+FLAC/100% loud?.wav", "/keysynth/_files/salamander/SFZ%2BFLAC/100%25%20loud%3F.wav")]
    public async Task FileInIndexIsAuthorisedViaXAccel(string path, string expectedAccel)
    {
        var res = await _f.CookieClient().GetAsync(SampleData.FileUrl("salamander", path));
        res.StatusCode.Should().Be(HttpStatusCode.OK, await res.Content.ReadAsStringAsync());
        res.Headers.GetValues("X-Accel-Redirect").Single().Should().Be(expectedAccel);
        res.Headers.CacheControl!.Private.Should().BeTrue();
        res.Headers.CacheControl.NoStore.Should().BeTrue();
        (await res.Content.ReadAsByteArrayAsync()).Should().BeEmpty();
        res.Content.Headers.ContentType.Should().BeNull("nginx must pick the type of the served file");
    }

    [Fact]
    public async Task DeviceCanFetchFiles()
    {
        var linked = await _f.LinkAsync();
        var res = await linked.Bearer.GetAsync(SampleData.FileUrl("salamander", "Sal/48khz24bit/A0v1.flac"));
        res.StatusCode.Should().Be(HttpStatusCode.OK);
        res.Headers.Contains("X-Accel-Redirect").Should().BeTrue();
    }

    [Theory]
    [InlineData("/api/samples/files/salamander/Sal/48khz24bit/a0v1.flac")]          // case differs
    [InlineData("/api/samples/files/salamander/Sal/48khz24bit/missing.flac")]       // not in index
    [InlineData("/api/samples/files/other/Sal/48khz24bit/A0v1.flac")]               // other library
    [InlineData("/api/samples/files/Salamander/Sal/48khz24bit/A0v1.flac")]          // invalid lib id
    [InlineData("/api/samples/files/salamander/Sal/%2E%2E/%2E%2E/index.json")]     // encoded traversal
    [InlineData("/api/samples/files/salamander/Sal%2F..%2F..%2Findex.json")]        // encoded slashes
    [InlineData("/api/samples/files/salamander/Sal%5C48khz24bit%5CA0v1.flac")]      // backslashes
    [InlineData("/api/samples/files/salamander/C:%2Fwindows%2Fwin.ini")]
    [InlineData("/api/samples/files/salamander/Sal//48khz24bit/A0v1.flac")]
    [InlineData("/api/samples/files/salamander/")]
    [InlineData("/api/samples/files/salamander/index.json")]
    public async Task NotInIndexOrInvalidIs404(string url)
    {
        var res = await _f.CookieClient().GetAsync(url);
        res.StatusCode.Should().Be(HttpStatusCode.NotFound, url);
        res.Headers.Contains("X-Accel-Redirect").Should().BeFalse();
    }

    [Fact]
    public async Task RawDotSegmentsAreNormalisedAwayOrRejected()
    {
        // HttpClient normalises "/../" itself; send the raw string to be sure nothing escapes.
        var res = await _f.CookieClient().SendAsync(new HttpRequestMessage(HttpMethod.Get,
            new Uri("/api/samples/files/salamander/Sal/48khz24bit/../../../../etc/passwd", UriKind.Relative)));
        res.Headers.Contains("X-Accel-Redirect").Should().BeFalse();
        res.StatusCode.Should().BeOneOf(HttpStatusCode.NotFound, HttpStatusCode.Unauthorized);
    }
}

public sealed class SampleIndexUnavailableTests
{
    [Fact]
    public async Task MissingOrInvalidIndexIs503()
    {
        await using var f = new KeysynthFactory();
        var client = f.CookieClient();
        await (await client.GetAsync("/api/samples/manifest")).ShouldBe(HttpStatusCode.ServiceUnavailable, "index_unavailable");
        await (await client.GetAsync(SampleData.FileUrl("salamander", "Sal/48khz24bit/A0v1.flac")))
            .ShouldBe(HttpStatusCode.ServiceUnavailable, "index_unavailable");

        f.WriteIndex("{ not json");
        await (await client.GetAsync("/api/samples/manifest")).ShouldBe(HttpStatusCode.ServiceUnavailable, "index_unavailable");

        f.WriteIndex(SampleData.Index);
        File.SetLastWriteTimeUtc(Path.Combine(f.SamplesRoot, "index.json"), DateTime.UtcNow.AddMinutes(1));
        (await client.GetAsync("/api/samples/manifest")).StatusCode.Should().Be(HttpStatusCode.OK);
    }

    [Fact]
    public async Task ManifestRateLimited60PerMinute()
    {
        await using var f = new KeysynthFactory();
        f.WriteIndex(SampleData.Index);
        var client = f.CookieClient();
        for (var i = 0; i < 60; i++)
            (await client.GetAsync("/api/samples/manifest")).StatusCode.Should().Be(HttpStatusCode.OK);
        await (await client.GetAsync("/api/samples/manifest")).ShouldBe(HttpStatusCode.TooManyRequests, "rate_limited");
    }
}

/// <summary>KS_DEV_SERVE_FILES=1: the API serves the bytes itself (no nginx locally), with ranges.</summary>
public sealed class DevServeFilesTests
{
    [Fact]
    public async Task ServesFileWithRange()
    {
        await using var f = new KeysynthFactory { DevServeFiles = true };
        f.WriteIndex(SampleData.Index);
        f.WriteSampleFile("salamander", "Сэмплы/до #1.wav", [1, 2, 3, 4, 5]);
        var client = f.CookieClient();

        var full = await client.GetAsync(SampleData.FileUrl("salamander", "Сэмплы/до #1.wav"));
        full.StatusCode.Should().Be(HttpStatusCode.OK);
        full.Headers.Contains("X-Accel-Redirect").Should().BeFalse();
        (await full.Content.ReadAsByteArrayAsync()).Should().Equal(1, 2, 3, 4, 5);

        var req = new HttpRequestMessage(HttpMethod.Get, SampleData.FileUrl("salamander", "Сэмплы/до #1.wav"));
        req.Headers.Range = new RangeHeaderValue(2, null);
        var part = await client.SendAsync(req);
        part.StatusCode.Should().Be(HttpStatusCode.PartialContent);
        (await part.Content.ReadAsByteArrayAsync()).Should().Equal(3, 4, 5);

        // in the index but not on disk → 404; not in the index → 404 even if on disk
        (await client.GetAsync(SampleData.FileUrl("salamander", "Sal/48khz24bit/A0v1.flac"))).StatusCode.Should().Be(HttpStatusCode.NotFound);
        f.WriteSampleFile("salamander", "secret.wav", [9]);
        (await client.GetAsync(SampleData.FileUrl("salamander", "secret.wav"))).StatusCode.Should().Be(HttpStatusCode.NotFound);
    }
}
