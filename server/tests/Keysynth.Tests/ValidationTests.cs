using FluentAssertions;
using Keysynth.Core;
using Xunit;

namespace Keysynth.Tests;

public class ValidationTests
{
    [Theory]
    [InlineData("warm-rhodes", true)]
    [InlineData("a", true)]
    [InlineData("a1-b2-c3", true)]
    [InlineData("", false)]
    [InlineData("-a", false)]
    [InlineData("a-", false)]
    [InlineData("a--b", false)]
    [InlineData("Warm", false)]
    [InlineData("a_b", false)]
    [InlineData("a.json", false)]
    [InlineData("a/b", false)]
    [InlineData("я", false)]
    public void Slugs(string slug, bool valid) => Core.Slugs.IsValid(slug).Should().Be(valid);

    [Fact]
    public void SlugLength()
    {
        Core.Slugs.IsValid(new string('a', 64)).Should().BeTrue();
        Core.Slugs.IsValid(new string('a', 65)).Should().BeFalse();
    }

    [Theory]
    [InlineData("3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40", true)]
    [InlineData("3F0C7E9A-5B1D-4C2E-9A77-0E5D2C1B8F40", false)] // lowercase only
    [InlineData("3f0c7e9a-5b1d-1c2e-9a77-0e5d2c1b8f40", false)] // not v4
    [InlineData("3f0c7e9a-5b1d-4c2e-7a77-0e5d2c1b8f40", false)] // bad variant
    [InlineData("3f0c7e9a5b1d4c2e9a770e5d2c1b8f40", false)]
    [InlineData("me", false)]
    public void DeviceIds(string id, bool valid) => Core.DeviceIds.IsValid(id).Should().Be(valid);

    [Theory]
    [InlineData("SalamanderGrandPiano-SFZ+FLAC-V3+20200602/48khz24bit/A0v1.flac", true)]
    [InlineData("Piano Samples/C4 soft.wav", true)]
    [InlineData("Сэмплы/до-диез #1.wav", true)]
    [InlineData("100%.wav", true)]
    [InlineData("a/b?c.wav", true)]
    [InlineData("", false)]
    [InlineData("/abs.wav", false)]
    [InlineData("a//b.wav", false)]
    [InlineData("a/", false)]
    [InlineData("../etc/passwd", false)]
    [InlineData("a/../../b", false)]
    [InlineData("a/./b", false)]
    [InlineData("a\\b.wav", false)]
    [InlineData("C:/x.wav", false)]
    [InlineData("a/b:stream", false)]
    [InlineData("a\u0001b", false)]
    [InlineData("a\u007Fb", false)]
    [InlineData("a/b./c", false)]
    [InlineData("a/b /c", false)]
    [InlineData("CON", false)]
    [InlineData("dir/con.wav", false)]
    [InlineData("dir/Lpt9.txt.wav", false)]
    [InlineData("dir/COM10.wav", true)]
    [InlineData("dir/CONSOLE.wav", true)]
    public void SamplePathRules(string path, bool valid) => SamplePaths.IsValidPath(path).Should().Be(valid);

    [Fact]
    public void SamplePathLengthAndSurrogates()
    {
        SamplePaths.IsValidPath(new string('a', 240)).Should().BeTrue();
        SamplePaths.IsValidPath(new string('a', 241)).Should().BeFalse();
        SamplePaths.IsValidPath("a\uD83C\uDFB9.wav").Should().BeTrue(); // 🎹
        SamplePaths.IsValidPath("a\uD83C.wav").Should().BeFalse();       // lone high surrogate
        SamplePaths.IsValidPath("a\uDFB9.wav").Should().BeFalse();       // lone low surrogate
    }

    [Theory]
    [InlineData("salamander", true)]
    [InlineData("vs-upright-1", true)]
    [InlineData("0lib", true)]
    [InlineData("-lib", false)]
    [InlineData("Lib", false)]
    [InlineData("a_b", false)]
    [InlineData("..", false)]
    [InlineData("", false)]
    public void LibraryIds(string id, bool valid) => SamplePaths.IsValidLibraryId(id).Should().Be(valid);
}
