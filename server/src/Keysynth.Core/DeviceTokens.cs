using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Keysynth.Core;

/// <summary>
/// Device bearer tokens (cloud-api.md §2.2): <c>ksd_</c> + base64url (no padding) of 32 CSPRNG bytes = 47 chars.
/// Only SHA-256(token string, UTF-8) is stored.
/// </summary>
public static partial class DeviceTokens
{
    public const string Prefix = "ksd_";
    public const int Length = 47;

    [GeneratedRegex("^ksd_[A-Za-z0-9_-]{43}$", RegexOptions.CultureInvariant)]
    private static partial Regex Pattern();

    public static string Generate()
    {
        Span<byte> bytes = stackalloc byte[32];
        RandomNumberGenerator.Fill(bytes);
        return Prefix + Base64Url(bytes);
    }

    public static bool IsWellFormed(string? token) => token is { Length: Length } && Pattern().IsMatch(token);

    public static byte[] Hash(string token) => SHA256.HashData(Encoding.UTF8.GetBytes(token));

    private static string Base64Url(ReadOnlySpan<byte> bytes) =>
        Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
}
