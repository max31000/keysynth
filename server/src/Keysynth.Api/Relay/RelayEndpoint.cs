using System.Text.RegularExpressions;
using Keysynth.Api.Auth;
using Keysynth.Api.Http;
using Keysynth.Core;
using Mvv42.Auth.Client;

namespace Keysynth.Api.Relay;

/// <summary>
/// <c>GET /api/relay?role=host|controller[&amp;cid=…]</c> WebSocket upgrade (docs/PROTOCOL.md *Relay*, cloud-api.md §3).
/// Every check happens before the upgrade, so a refusal is a plain HTTP status with an <c>{error}</c> body:
/// both a cookie and a bearer → 400; host = device bearer only (failures keep their 401 code, a cookie → 403);
/// controller = granted mvv42 cookie only with an allowed <c>Origin</c> (missing / foreign / bearer → 403).
/// The endpoint opts out of the policy check and reads the identities the fallback policy authenticated.
/// </summary>
public static partial class RelayEndpoint
{
    [GeneratedRegex("^[A-Za-z0-9_-]{8,64}$", RegexOptions.CultureInvariant)]
    private static partial Regex CidPattern();

    public static IServiceCollection AddRelay(this IServiceCollection services, IConfiguration configuration)
    {
        services.Configure<RelayOptions>(configuration.GetSection("Relay"));
        services.AddSingleton<RelayHub>();
        services.AddHostedService(sp => sp.GetRequiredService<RelayHub>());
        return services;
    }

    public static void MapRelayEndpoint(this IEndpointRouteBuilder app) =>
        app.MapGet("/api/relay", Connect).AllowAnonymous();

    private static async Task<IResult> Connect(HttpContext ctx, RelayHub hub, Mvv42AuthOptions mvv42)
    {
        var request = ctx.Request;
        var role = request.Query["role"].ToString();
        var hasCookie = request.Cookies.ContainsKey(mvv42.CookieName);
        var hasBearer = request.Headers.Authorization.Count > 0;
        if (role is not ("host" or "controller") || request.Query["role"].Count != 1)
            return Errors.InvalidRequest;
        if (hasCookie && hasBearer)
            return Errors.InvalidRequest;

        long userId;
        string id;
        byte[]? tokenHash = null;
        if (role == "host")
        {
            if (hasCookie)
                return Errors.Of(403, "forbidden");
            if (ctx.Items.TryGetValue(KeysynthAuth.DeviceErrorItem, out var v) && v is string deviceError)
            {
                ctx.Response.Headers.WWWAuthenticate = $"Bearer error=\"{deviceError}\"";
                return Errors.Of(401, deviceError);
            }
            if (KeysynthAuth.DevicePrincipal(ctx.User) is not { } device)
                return Errors.Of(401, "unauthorized");
            userId = device.UserId;
            id = device.DeviceId;
            // Header shape already validated by the device scheme.
            tokenHash = DeviceTokens.Hash(request.Headers.Authorization[0]!["Bearer ".Length..].Trim());
        }
        else
        {
            if (hasBearer || !IsAllowedOrigin(request, mvv42))
                return Errors.Of(403, "forbidden");
            if (!hasCookie || ctx.User.Identities.All(i => !(i.IsAuthenticated && i.AuthenticationType == Mvv42Auth.Scheme)))
                return Errors.Of(401, "unauthorized");
            if (KeysynthAuth.GrantedCookieUser(ctx.User) is null)
                return Errors.Of(403, "forbidden");
            userId = KeysynthAuth.RequireCookieUserId(ctx);
            var cid = request.Query["cid"];
            if (cid.Count > 1 || (cid.Count == 1 && !CidPattern().IsMatch(cid[0] ?? "")))
                return Errors.InvalidRequest;
            id = cid.Count == 1 ? cid[0]! : Guid.NewGuid().ToString("D");
        }

        if (!ctx.WebSockets.IsWebSocketRequest)
            return Errors.Of(400, "websocket_required");

        using var socket = await ctx.WebSockets.AcceptWebSocketAsync();
        var conn = new RelayConnection(role == "host" ? RelayRole.Host : RelayRole.Controller, userId, id, tokenHash, socket, hub.Options);
        await hub.RunAsync(conn, socket, ctx.RequestAborted);
        return Results.Empty;
    }

    /// <summary>
    /// The SDK's Origin filter only covers unsafe methods; a WS upgrade is a GET, so the controller handshake needs
    /// an exact <c>Origin</c> from <see cref="Mvv42AuthOptions.AllowedOrigins"/> (no Referer fallback).
    /// </summary>
    private static bool IsAllowedOrigin(HttpRequest request, Mvv42AuthOptions mvv42)
    {
        if (request.Headers.Origin.Count != 1)
            return false;
        var origin = request.Headers.Origin[0];
        return !string.IsNullOrEmpty(origin)
            && mvv42.AllowedOrigins.Any(o => string.Equals(o.TrimEnd('/'), origin, StringComparison.OrdinalIgnoreCase));
    }
}
