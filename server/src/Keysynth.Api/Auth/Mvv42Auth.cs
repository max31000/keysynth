// mvv42-auth SDK for .NET 8 apps — copy this single file into the app (e.g. src/<App>.Api/Auth/Mvv42Auth.cs).
// Source of truth: https://github.com/max31000/mvv42-auth/blob/main/sdk/dotnet/Mvv42Auth.cs
// Requires the package Microsoft.AspNetCore.Authentication.JwtBearer (8.0.x, 8.0.11 recommended). All
// Microsoft.IdentityModel.* / System.IdentityModel.Tokens.Jwt packages of the app must resolve to ONE version
// (tested with 7.1.2, 7.6.3, 8.4.0): pin Microsoft.IdentityModel.Protocols.OpenIdConnect to the app's
// System.IdentityModel.Tokens.Jwt version. Dual-mode policy: sdk/dotnet/README.md (never scheme-only).
//
//   builder.Services.AddMvv42Auth("bonds", o => builder.Configuration.GetSection("Mvv42Auth").Bind(o));
//   app.UseAuthentication(); app.UseAuthorization();
//   app.MapGet("/api/x", (ClaimsPrincipal u) => u.GetMvv42User()).RequireAuthorization(Mvv42Auth.Policy);
//
// See sdk/dotnet/README.md and docs/migration.md.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net.Http;
using System.Security.Claims;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.AspNetCore.Authentication.JwtBearer;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.IdentityModel.Protocols;
using Microsoft.IdentityModel.Protocols.OpenIdConnect;
using Microsoft.IdentityModel.Tokens;

namespace Mvv42.Auth.Client;

public static class Mvv42Auth
{
    /// <summary>Authentication scheme (sits next to an app's legacy Bearer scheme during dual-mode).</summary>
    public const string Scheme = "mvv42";

    /// <summary>Authorization policy: authenticated via <see cref="Scheme"/> and (adm == "true" or appId ∈ apps).</summary>
    public const string Policy = "mvv42:app";

    /// <summary>
    /// Registers the <c>mvv42</c> JwtBearer scheme (token only from the access cookie, keys from JWKS),
    /// the <c>mvv42:app</c> policy and the Origin check for cookie-authenticated unsafe requests.
    /// Does not change the app's default scheme.
    /// </summary>
    public static IServiceCollection AddMvv42Auth(this IServiceCollection services, string appId, Action<Mvv42AuthOptions>? configure = null)
    {
        var options = new Mvv42AuthOptions();
        configure?.Invoke(options);
        services.AddSingleton(options);

        var httpClient = options.BackchannelHandler is null ? new HttpClient() : new HttpClient(options.BackchannelHandler);
        httpClient.Timeout = TimeSpan.FromSeconds(10);
        var jwks = new ConfigurationManager<JsonWebKeySet>(
            options.JwksUrl,
            new JwksRetriever(),
            new HttpDocumentRetriever(httpClient) { RequireHttps = options.RequireHttpsMetadata })
        {
            AutomaticRefreshInterval = options.JwksRefreshInterval,
            RefreshInterval = TimeSpan.FromSeconds(30), // minimum gap between refreshes forced by an unknown kid
        };

        services.AddAuthentication().AddJwtBearer(Scheme, o =>
        {
            o.MapInboundClaims = false;
            o.RequireHttpsMetadata = options.RequireHttpsMetadata;
            o.ConfigurationManager = new JwksConfigurationManager(jwks, options.Issuer);
            o.TokenValidationParameters = new TokenValidationParameters
            {
                ValidIssuer = options.Issuer,
                ValidAudience = options.Audience,
                ValidAlgorithms = [SecurityAlgorithms.EcdsaSha256],
                ClockSkew = TimeSpan.FromSeconds(30),
                AuthenticationType = Scheme,
                NameClaimType = "name",
            };
            o.Events = new JwtBearerEvents
            {
                // Cookie only: an Authorization header belongs to the app's legacy scheme, not to us.
                OnMessageReceived = ctx =>
                {
                    ctx.Token = ctx.Request.Cookies[options.CookieName];
                    if (string.IsNullOrEmpty(ctx.Token))
                        ctx.NoResult(); // otherwise JwtBearer falls back to the Authorization header
                    return Task.CompletedTask;
                },
            };
        });

        services.AddAuthorization(o => o.AddPolicy(Policy, p => p
            .AddAuthenticationSchemes(Scheme)
            .RequireAuthenticatedUser()
            .RequireAssertion(ctx => ctx.User.HasClaim("adm", "true") || ctx.User.HasClaim("apps", appId))));

        if (options.EnforceOrigin)
            services.AddSingleton<IStartupFilter>(new OriginCheckStartupFilter(options));

        return services;
    }

    /// <summary>The mvv42 user of a request authenticated via <see cref="Scheme"/>; null otherwise (e.g. legacy scheme).</summary>
    public static Mvv42User? GetMvv42User(this ClaimsPrincipal principal)
    {
        var identity = principal.Identities.FirstOrDefault(i => i.IsAuthenticated && i.AuthenticationType == Scheme);
        var sub = identity?.FindFirst("sub")?.Value;
        if (identity is null || sub is null
            || !long.TryParse(identity.FindFirst("tg")?.Value, NumberStyles.None, CultureInfo.InvariantCulture, out var telegramId))
            return null;

        return new Mvv42User(sub, telegramId, identity.FindFirst("name")?.Value ?? "", identity.HasClaim("adm", "true"));
    }

    /// <summary>JWKS document → <see cref="JsonWebKeySet"/> (no OIDC discovery involved).</summary>
    private sealed class JwksRetriever : IConfigurationRetriever<JsonWebKeySet>
    {
        public async Task<JsonWebKeySet> GetConfigurationAsync(string address, IDocumentRetriever retriever, CancellationToken cancel) =>
            new(await retriever.GetDocumentAsync(address, cancel).ConfigureAwait(false));
    }

    /// <summary>
    /// Adapts the JWKS manager to what JwtBearer consumes. Deliberately not a BaseConfigurationManager: JwtBearer then
    /// merges <see cref="OpenIdConnectConfiguration.SigningKeys"/> into the validation parameters and calls
    /// <see cref="RequestRefresh"/> when a token carries an unknown kid (key rotation).
    /// </summary>
    private sealed class JwksConfigurationManager(ConfigurationManager<JsonWebKeySet> inner, string issuer)
        : IConfigurationManager<OpenIdConnectConfiguration>
    {
        /// <summary>Source JWKS and the configuration built from it, swapped as one immutable reference.</summary>
        private sealed record Snapshot(JsonWebKeySet Source, OpenIdConnectConfiguration Config);

        private volatile Snapshot? _snapshot;

        public async Task<OpenIdConnectConfiguration> GetConfigurationAsync(CancellationToken cancel)
        {
            var jwks = await inner.GetConfigurationAsync(cancel).ConfigureAwait(false);
            var current = _snapshot;
            if (current is not null && ReferenceEquals(current.Source, jwks))
                return current.Config;

            var config = new OpenIdConnectConfiguration { Issuer = issuer, JsonWebKeySet = jwks };
            foreach (var key in jwks.GetSigningKeys())
                config.SigningKeys.Add(key);
            _snapshot = new Snapshot(jwks, config); // a concurrent caller may build the same one: harmless
            return config;
        }

        public void RequestRefresh() => inner.RequestRefresh();
    }

    /// <summary>
    /// CSRF defence in depth: a POST/PUT/PATCH/DELETE that carries the mvv42 access cookie must come with an
    /// <c>Origin</c> (or <c>Referer</c>) from <see cref="Mvv42AuthOptions.AllowedOrigins"/>, else 403.
    /// Requests without the cookie are left alone (no ambient credential, nothing to forge).
    /// </summary>
    private sealed class OriginCheckStartupFilter(Mvv42AuthOptions options) : IStartupFilter
    {
        public Action<IApplicationBuilder> Configure(Action<IApplicationBuilder> next) => app =>
        {
            app.Use(async (ctx, nextMiddleware) =>
            {
                var request = ctx.Request;
                var unsafeMethod = !(HttpMethods.IsGet(request.Method) || HttpMethods.IsHead(request.Method)
                                     || HttpMethods.IsOptions(request.Method) || HttpMethods.IsTrace(request.Method));
                if (unsafeMethod && request.Cookies.ContainsKey(options.CookieName) && !IsAllowedOrigin(request))
                {
                    ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
                    ctx.Response.ContentType = "application/json";
                    await ctx.Response.WriteAsync("{\"error\":\"origin_not_allowed\"}");
                    return;
                }
                await nextMiddleware(ctx);
            });
            next(app);
        };

        private bool IsAllowedOrigin(HttpRequest request)
        {
            string origin = request.Headers.Origin.ToString();
            if (string.IsNullOrEmpty(origin))
            {
                if (!Uri.TryCreate(request.Headers.Referer.ToString(), UriKind.Absolute, out var referer))
                    return false;
                origin = referer.GetLeftPart(UriPartial.Authority);
            }
            origin = origin.TrimEnd('/');
            return options.AllowedOrigins.Any(o => string.Equals(o.TrimEnd('/'), origin, StringComparison.OrdinalIgnoreCase));
        }
    }
}

public sealed class Mvv42AuthOptions
{
    /// <summary>Fetched over the internal docker network (the app container must be in <c>cashpulse-network</c>).</summary>
    public string JwksUrl { get; set; } = "http://mvv42-auth:8080/auth/.well-known/jwks.json";

    public string Issuer { get; set; } = "https://mvv42.ru/auth";
    public string Audience { get; set; } = "mvv42";

    /// <summary>Production name; in local Development the auth service uses <c>mvv42_at</c>.</summary>
    public string CookieName { get; set; } = "__Host-mvv42_at";

    /// <summary>false: the JWKS URL is plain http inside the docker network.</summary>
    public bool RequireHttpsMetadata { get; set; }

    /// <summary>Periodic JWKS re-fetch; shorter than the 15-minute publish-before-sign delay of new keys.</summary>
    public TimeSpan JwksRefreshInterval { get; set; } = TimeSpan.FromMinutes(10);

    public bool EnforceOrigin { get; set; } = true;
    public List<string> AllowedOrigins { get; set; } = ["https://mvv42.ru"];

    /// <summary>Tests only: handler for JWKS requests.</summary>
    public HttpMessageHandler? BackchannelHandler { get; set; }
}

/// <param name="AuthUserId">mvv42-auth user id (<c>sub</c>).</param>
/// <param name="TelegramId">Telegram id (<c>tg</c>): the key for finding/creating the app's local user.</param>
public sealed record Mvv42User(string AuthUserId, long TelegramId, string Name, bool IsAdmin);
