using System.Security.Cryptography;
using System.Text;
using FluentAssertions;
using Keysynth.Core;
using Keysynth.Infrastructure;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;
using Xunit;

namespace Keysynth.Tests;

public class SampleIndexTests
{
    internal const string Json = """
        {
          "format": 1,
          "generatedAt": "2026-10-12T12:00:00Z",
          "groups": [ { "id": "piano", "name": "Piano" } ],
          "libraries": [
            { "id": "salamander", "name": "S", "group": "piano", "bytes": 3,
              "files": [ { "path": "Sal/A0v1.flac", "bytes": 1, "sha256": "00" },
                         { "path": "Sal/Piano Samples/C4 soft.wav", "bytes": 1, "sha256": "00" },
                         { "path": "../escape.wav", "bytes": 1, "sha256": "00" } ],
              "entries": [] },
            { "id": "upright", "name": "U", "group": "piano", "bytes": 1,
              "files": [ { "path": "Сэмпл/C#4.wav", "bytes": 1, "sha256": "00" } ], "entries": [] }
          ]
        }
        """;

    [Fact]
    public void ParsesFilesPerLibraryExactly()
    {
        var index = SampleIndex.Parse(Encoding.UTF8.GetBytes(Json));
        index.Contains("salamander", "Sal/A0v1.flac").Should().BeTrue();
        index.Contains("salamander", "Sal/Piano Samples/C4 soft.wav").Should().BeTrue();
        index.Contains("upright", "Сэмпл/C#4.wav").Should().BeTrue();

        index.Contains("salamander", "sal/a0v1.flac").Should().BeFalse("case-sensitive");
        index.Contains("upright", "Sal/A0v1.flac").Should().BeFalse("other library");
        index.Contains("nope", "Sal/A0v1.flac").Should().BeFalse();
        index.Contains("salamander", "../escape.wav").Should().BeFalse("invalid paths are never servable");
        index.SkippedPaths.Should().Be(1);
    }

    [Fact]
    public void ETagIsFirst16HexOfSha256Quoted()
    {
        var bytes = Encoding.UTF8.GetBytes(Json);
        var index = SampleIndex.Parse(bytes);
        var hex = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        index.ETag.Should().Be("\"" + hex[..16] + "\"");
        index.Bytes.Should().Equal(bytes);
    }

    [Theory]
    [InlineData("not json")]
    [InlineData("[]")]
    [InlineData("""{"libraries": []}""")]
    [InlineData("""{"format": "1", "libraries": []}""")]
    [InlineData("""{"format": 1, "libraries": [ { "id": "Bad", "files": [] } ]}""")]
    [InlineData("""{"format": 1, "libraries": [ { "id": "a", "files": [] }, { "id": "a", "files": [] } ]}""")]
    [InlineData("""{"format": 1, "libraries": [ { "id": "a", "files": [ { "path": 1 } ] } ]}""")]
    public void RejectsInvalid(string json) =>
        FluentActions.Invoking(() => SampleIndex.Parse(Encoding.UTF8.GetBytes(json))).Should().Throw<FormatException>();
}

public sealed class SampleIndexProviderTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("ks-index-").FullName;

    public void Dispose() => Directory.Delete(_root, recursive: true);

    private SampleIndexProvider Provider() =>
        new(Options.Create(new KeysynthOptions { SamplesRoot = _root }), NullLogger<SampleIndexProvider>.Instance);

    private string IndexPath => Path.Combine(_root, "index.json");

    [Fact]
    public void MissingThenLoadedThenReloadedOnChange()
    {
        var provider = Provider();
        provider.Current.Should().BeNull();

        File.WriteAllText(IndexPath, SampleIndexTests.Json);
        var first = provider.Current;
        first.Should().NotBeNull();
        provider.Current.Should().BeSameAs(first, "unchanged file is not re-parsed");

        File.WriteAllText(IndexPath, """{"format": 1, "libraries": [ { "id": "other", "files": [ { "path": "x.wav" } ] } ]}""");
        File.SetLastWriteTimeUtc(IndexPath, DateTime.UtcNow.AddMinutes(1));
        var second = provider.Current;
        second.Should().NotBeNull().And.NotBeSameAs(first);
        second!.Contains("other", "x.wav").Should().BeTrue();
        second.ETag.Should().NotBe(first!.ETag);
    }

    [Fact]
    public void InvalidFileIsUnavailableUntilFixed()
    {
        var provider = Provider();
        File.WriteAllText(IndexPath, "{ broken");
        provider.Current.Should().BeNull();

        File.WriteAllText(IndexPath, SampleIndexTests.Json);
        File.SetLastWriteTimeUtc(IndexPath, DateTime.UtcNow.AddMinutes(1));
        provider.Current.Should().NotBeNull();

        File.Delete(IndexPath);
        provider.Current.Should().BeNull();
    }
}

public class MigrationRunnerTests
{
    [Fact]
    public void SplitIgnoresSemicolonsInLineComments()
    {
        var parts = MigrationRunner.SplitSqlStatements("""
            -- a comment; with a semicolon
            CREATE TABLE a (x INT); -- trailing; comment
            CREATE TABLE b (y INT);
            """);
        parts.Should().Equal("CREATE TABLE a (x INT)", "CREATE TABLE b (y INT)");
    }

    [Fact]
    public void DiscoversEmbeddedMigrationsInOrder()
    {
        var found = MigrationRunner.Discover();
        found.Should().NotBeEmpty();
        found[0].Version.Should().Be(1);
        found.Select(f => f.Version).Should().BeInAscendingOrder().And.OnlyHaveUniqueItems();
    }

    [Fact]
    public void InitialMigrationMatchesContractTables()
    {
        using var stream = typeof(MigrationRunner).Assembly.GetManifestResourceStream(MigrationRunner.Discover()[0].Resource)!;
        var sql = new StreamReader(stream).ReadToEnd();
        var statements = MigrationRunner.SplitSqlStatements(sql);
        statements.Should().HaveCount(3);
        statements.Should().Contain(s => s.StartsWith("CREATE TABLE users"))
            .And.Contain(s => s.StartsWith("CREATE TABLE devices") && s.Contains("token_hash    BINARY(32)"))
            .And.Contain(s => s.StartsWith("CREATE TABLE presets") && s.Contains("ascii_bin"));
    }

    [Fact]
    public void SongsMigrationMatchesContractTable()
    {
        var (version, resource) = MigrationRunner.Discover().Single(m => m.Resource.EndsWith("002_songs.sql", StringComparison.Ordinal));
        version.Should().Be(2);
        using var stream = typeof(MigrationRunner).Assembly.GetManifestResourceStream(resource)!;
        var statements = MigrationRunner.SplitSqlStatements(new StreamReader(stream).ReadToEnd());
        statements.Should().ContainSingle().Which.Should().StartWith("CREATE TABLE songs")
            .And.Contain("ascii_bin").And.Contain("song        MEDIUMTEXT    NULL").And.Contain("PRIMARY KEY (user_id, slug)");
    }
}
