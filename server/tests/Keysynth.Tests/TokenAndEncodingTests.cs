using System.Security.Cryptography;
using System.Text;
using FluentAssertions;
using Keysynth.Api.Http;
using Keysynth.Core;
using Xunit;

namespace Keysynth.Tests;

public class DeviceTokensTests
{
    [Fact]
    public void GeneratedTokensAreWellFormedAndUnique()
    {
        var tokens = Enumerable.Range(0, 200).Select(_ => DeviceTokens.Generate()).ToList();
        tokens.Should().OnlyHaveUniqueItems();
        tokens.Should().AllSatisfy(t =>
        {
            t.Should().HaveLength(47).And.StartWith("ksd_");
            DeviceTokens.IsWellFormed(t).Should().BeTrue();
        });
    }

    [Theory]
    [InlineData("")]
    [InlineData("ksd_")]
    [InlineData("ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")]   // 42
    [InlineData("ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")] // 44
    [InlineData("ksx_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")]
    [InlineData("ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")]
    [InlineData("ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA+")]
    public void RejectsMalformed(string token) => DeviceTokens.IsWellFormed(token).Should().BeFalse();

    [Fact]
    public void HashIsSha256OfUtf8String()
    {
        const string token = "ksd_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        DeviceTokens.Hash(token).Should().Equal(SHA256.HashData(Encoding.UTF8.GetBytes(token)));
        DeviceTokens.Hash(token).Should().HaveCount(32);
    }
}

public class AccelPathTests
{
    [Theory]
    [InlineData("salamander", "Salamander/48khz24bit/A0v1.flac", "/keysynth/_files/salamander/Salamander/48khz24bit/A0v1.flac")]
    [InlineData("lib", "Piano Samples/C4 soft.wav", "/keysynth/_files/lib/Piano%20Samples/C4%20soft.wav")]
    [InlineData("lib", "Сэмпл/я.wav", "/keysynth/_files/lib/%D0%A1%D1%8D%D0%BC%D0%BF%D0%BB/%D1%8F.wav")]
    [InlineData("lib", "C#4.wav", "/keysynth/_files/lib/C%234.wav")]
    [InlineData("lib", "a?b.wav", "/keysynth/_files/lib/a%3Fb.wav")]
    [InlineData("lib", "100%.wav", "/keysynth/_files/lib/100%25.wav")]
    [InlineData("lib", "SFZ+FLAC/x.sfz", "/keysynth/_files/lib/SFZ%2BFLAC/x.sfz")]
    [InlineData("lib", "a~b_c-d.e", "/keysynth/_files/lib/a~b_c-d.e")]
    [InlineData("lib", "(1)&'!*,;=$@.wav", "/keysynth/_files/lib/%281%29%26%27%21%2A%2C%3B%3D%24%40.wav")]
    public void EncodesEverySegment(string lib, string path, string expected) =>
        AccelPath.For(lib, path).Should().Be(expected);

    [Fact]
    public void RoundTripsThroughUnescape()
    {
        const string path = "Сэмплы #2/100% +soft/C 4.wav";
        var encoded = AccelPath.For("lib", path);
        Uri.UnescapeDataString(encoded).Should().Be("/keysynth/_files/lib/" + path);
        encoded.Should().MatchRegex("^[A-Za-z0-9/%._~-]+$");
    }
}

public class ETagsTests
{
    [Theory]
    [InlineData("\"abc\"", true)]
    [InlineData("W/\"abc\"", true)]
    [InlineData("\"x\", \"abc\"", true)]
    [InlineData("*", true)]
    [InlineData("\"abd\"", false)]
    [InlineData("abc", false)]
    [InlineData("", false)]
    public void NoneMatch(string header, bool hit) => ETags.NoneMatchHits(header, "\"abc\"").Should().Be(hit);

    [Theory]
    [InlineData("\"4\"", 4u)]
    [InlineData(" \"12\" ", 12u)]
    [InlineData("\"4294967295\"", 4294967295u)]
    [InlineData("4", null)]
    [InlineData("W/\"4\"", null)]
    [InlineData("\"4294967296\"", null)]
    [InlineData("\"-1\"", null)]
    [InlineData("\"\"", null)]
    [InlineData("*", null)]
    [InlineData("\"1\", \"2\"", null)]
    public void ParseRevision(string header, uint? expected) => ETags.ParseRevision(header).Should().Be(expected);
}

public class TruncateTests
{
    [Fact]
    public void NeverSplitsSurrogatePairs()
    {
        Keysynth.Api.Auth.KeysynthAuth.Truncate("ab🎹", 3).Should().Be("ab");
        Keysynth.Api.Auth.KeysynthAuth.Truncate("ab🎹", 4).Should().Be("ab🎹");
        Keysynth.Api.Auth.KeysynthAuth.Truncate("abcdef", 3).Should().Be("abc");
    }
}
