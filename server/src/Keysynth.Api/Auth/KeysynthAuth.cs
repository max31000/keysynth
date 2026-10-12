using System.Globalization;
using System.Security.Claims;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Authorization.Policy;
using Mvv42.Auth.Client;

namespace Keysynth.Api.Auth;

/// <summary>
/// Auth schemes and policies (cloud-api.md §2.1). Every policy authenticates with BOTH schemes and then requires
/// the identity it accepts, so the wrong scheme for an endpoint is 403 <c>forbidden</c> (not 401) and failures of a
/// bearer token keep their own code (<see cref="KeysynthAuthResultHandler"/>).
/// </summary>
public static class KeysynthAuth
{
    public const string AppId = "keysynth";
    public const string DeviceScheme = "device";

    /// <summary>mvv42 cookie user with the keysynth grant (= <see cref="Mvv42Auth.Policy"/>).</summary>
    public const string CookiePolicy = "Cookie";

    /// <summary>Device bearer token.</summary>
    public const string DevicePolicy = "Device";

    /// <summary><see cref="CookiePolicy"/> OR <see cref="DevicePolicy"/>.</summary>
    public const string AnyUserPolicy = "AnyUser";

    public const string UserIdClaim = "ks:uid";
    public const string DeviceIdClaim = "ks:device";

    internal const string UserIdItem = "ks.userId";
    internal const string DeviceErrorItem = "ks.deviceError";

    public static IServiceCollection AddKeysynthAuth(this IServiceCollection services, IConfiguration configuration)
    {
        services.AddMvv42Auth(AppId, o => configuration.GetSection("Mvv42Auth").Bind(o));
        services.AddAuthentication()
            .AddScheme<AuthenticationSchemeOptions, DeviceTokenHandler>(DeviceScheme, _ => { });

        services.AddSingleton<IAuthorizationMiddlewareResultHandler, KeysynthAuthResultHandler>();
        services.AddAuthorization(o =>
        {
            AuthorizationPolicy Build(Func<ClaimsPrincipal, bool> accept) => new AuthorizationPolicyBuilder(Mvv42Auth.Scheme, DeviceScheme)
                .RequireAuthenticatedUser()
                .RequireAssertion(ctx => accept(ctx.User))
                .Build();

            o.AddPolicy(CookiePolicy, Build(HasGrantedCookieUser));
            o.AddPolicy(DevicePolicy, Build(HasDevice));
            o.AddPolicy(AnyUserPolicy, Build(u => HasDevice(u) || HasGrantedCookieUser(u)));
            // New endpoints are protected unless they opt out explicitly (only /health does).
            o.FallbackPolicy = o.GetPolicy(AnyUserPolicy);
            o.DefaultPolicy = o.GetPolicy(AnyUserPolicy)!;
        });

        services.AddMemoryCache();
        services.AddSingleton<ProvisioningCache>();
        services.AddScoped<UserProvisioning>();
        return services;
    }

    private static ClaimsIdentity? Identity(ClaimsPrincipal user, string type) =>
        user.Identities.FirstOrDefault(i => i.IsAuthenticated && i.AuthenticationType == type);

    public static bool HasDevice(ClaimsPrincipal user) => Identity(user, DeviceScheme) is not null;

    /// <summary>The mvv42 cookie user if the token passes <c>mvv42:app</c> (adm == "true" or keysynth ∈ apps).</summary>
    public static Mvv42User? GrantedCookieUser(ClaimsPrincipal user)
    {
        var identity = Identity(user, Mvv42Auth.Scheme);
        if (identity is null || !(identity.HasClaim("adm", "true") || identity.HasClaim("apps", AppId)))
            return null;
        return user.GetMvv42User();
    }

    private static bool HasGrantedCookieUser(ClaimsPrincipal user) => GrantedCookieUser(user) is not null;

    /// <summary>The device principal's (userId, deviceId), if the request is authenticated by a device token.</summary>
    public static (long UserId, string DeviceId)? DevicePrincipal(ClaimsPrincipal user)
    {
        var identity = Identity(user, DeviceScheme);
        if (identity is null
            || !long.TryParse(identity.FindFirst(UserIdClaim)?.Value, NumberStyles.None, CultureInfo.InvariantCulture, out var uid)
            || identity.FindFirst(DeviceIdClaim)?.Value is not { } did)
            return null;
        return (uid, did);
    }

    /// <summary>Local <c>users.id</c> of the request: device claim, else the provisioned cookie user.</summary>
    public static long? UserId(HttpContext ctx) =>
        DevicePrincipal(ctx.User)?.UserId ?? (ctx.Items.TryGetValue(UserIdItem, out var v) && v is long id ? id : null);

    public static long RequireUserId(HttpContext ctx) =>
        UserId(ctx) ?? throw new InvalidOperationException("authorized request without a local user");
}

/// <summary>
/// Writes auth failures in the contract's shape instead of running the schemes' challenges:
/// a failed bearer → 401 with its code (<c>token_invalid</c> / <c>token_expired</c> / <c>reauth_required</c>) and
/// <c>WWW-Authenticate: Bearer error="…"</c>; no credentials → 401 <c>unauthorized</c>; authenticated but not accepted
/// by the policy (no grant, wrong scheme) → 403 <c>forbidden</c>.
/// </summary>
public sealed class KeysynthAuthResultHandler : IAuthorizationMiddlewareResultHandler
{
    private readonly AuthorizationMiddlewareResultHandler _default = new();

    public async Task HandleAsync(RequestDelegate next, HttpContext context, AuthorizationPolicy policy, PolicyAuthorizationResult result)
    {
        if (result.Succeeded)
        {
            await _default.HandleAsync(next, context, policy, result);
            return;
        }

        if (context.Items.TryGetValue(KeysynthAuth.DeviceErrorItem, out var v) && v is string deviceError)
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            context.Response.Headers.WWWAuthenticate = $"Bearer error=\"{deviceError}\"";
            await context.Response.WriteAsJsonAsync(new { error = deviceError });
            return;
        }

        if (result.Forbidden)
        {
            context.Response.StatusCode = StatusCodes.Status403Forbidden;
            await context.Response.WriteAsJsonAsync(new { error = "forbidden" });
            return;
        }

        context.Response.StatusCode = StatusCodes.Status401Unauthorized;
        if (context.Request.Headers.Authorization.Count > 0)
            context.Response.Headers.WWWAuthenticate = "Bearer error=\"unauthorized\"";
        await context.Response.WriteAsJsonAsync(new { error = "unauthorized" });
    }
}
