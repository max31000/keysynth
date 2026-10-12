using System.Globalization;
using System.Security.Cryptography;
using Microsoft.AspNetCore.Authentication.JwtBearer;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.IdentityModel.JsonWebTokens;
using Microsoft.IdentityModel.Protocols;
using Microsoft.IdentityModel.Protocols.OpenIdConnect;
using Microsoft.IdentityModel.Tokens;
using Mvv42.Auth.Client;

namespace Keysynth.IntegrationTests.Infrastructure;

/// <summary>
/// Test side of the shared mvv42 auth (pattern: bonds tests/…/Mvv42TestAuth.cs): an ES256 key standing in for the JWKS
/// and access tokens shaped like mvv42-auth's (sub, tg, name, adm, apps), sent in the production cookie.
/// </summary>
public static class Mvv42TestAuth
{
    public const string CookieName = "__Host-mvv42_at";
    public const string AllowedOrigin = "https://mvv42.ru";
    private const string Issuer = "https://mvv42.ru/auth";
    private const string Audience = "mvv42";

    private static readonly ECDsaSecurityKey SigningKey = new(ECDsa.Create(ECCurve.NamedCurves.nistP256)) { KeyId = "test-es256" };

    public static void UseTestSigningKey(IServiceCollection services) =>
        services.PostConfigure<JwtBearerOptions>(Mvv42Auth.Scheme, o =>
        {
            var config = new OpenIdConnectConfiguration { Issuer = Issuer };
            config.SigningKeys.Add(SigningKey);
            o.ConfigurationManager = new StaticConfigurationManager<OpenIdConnectConfiguration>(config);
        });

    public static string CreateToken(
        long telegramId,
        string name = "Max",
        IReadOnlyList<string>? apps = null,
        bool isAdmin = false,
        string? authUserId = null,
        TimeSpan? lifetime = null)
    {
        var now = DateTime.UtcNow;
        var expires = now.Add(lifetime ?? TimeSpan.FromMinutes(10));
        return new JsonWebTokenHandler().CreateToken(new SecurityTokenDescriptor
        {
            Issuer = Issuer,
            Audience = Audience,
            IssuedAt = expires < now ? expires.AddMinutes(-10) : now,
            NotBefore = expires < now ? expires.AddMinutes(-10) : now,
            Expires = expires,
            SigningCredentials = new SigningCredentials(SigningKey, SecurityAlgorithms.EcdsaSha256),
            Claims = new Dictionary<string, object>
            {
                [JwtRegisteredClaimNames.Sub] = authUserId ?? ("auth-" + telegramId.ToString(CultureInfo.InvariantCulture)),
                ["tg"] = telegramId.ToString(CultureInfo.InvariantCulture),
                ["name"] = name,
                ["adm"] = isAdmin ? "true" : "false",
                ["apps"] = (apps ?? ["keysynth"]).ToList(),
            },
        });
    }

    /// <summary>Cookie + allowed Origin on every request of <paramref name="client"/>.</summary>
    public static HttpClient WithCookie(this HttpClient client, string token)
    {
        client.DefaultRequestHeaders.Remove("Cookie");
        client.DefaultRequestHeaders.Add("Cookie", $"{CookieName}={token}");
        client.DefaultRequestHeaders.Remove("Origin");
        client.DefaultRequestHeaders.Add("Origin", AllowedOrigin);
        return client;
    }

    public static HttpClient WithBearer(this HttpClient client, string token)
    {
        client.DefaultRequestHeaders.Remove("Authorization");
        client.DefaultRequestHeaders.TryAddWithoutValidation("Authorization", "Bearer " + token);
        return client;
    }
}
