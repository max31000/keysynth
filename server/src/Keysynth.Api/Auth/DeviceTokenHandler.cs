using System.Globalization;
using System.Security.Claims;
using System.Security.Cryptography;
using System.Text.Encodings.Web;
using Keysynth.Core;
using Microsoft.AspNetCore.Authentication;
using Microsoft.Extensions.Options;

namespace Keysynth.Api.Auth;

/// <summary>
/// <c>device</c> scheme (cloud-api.md §2.2–2.3): <c>Authorization: Bearer ksd_…</c>. Check order: format → hash
/// lookup (<c>token_invalid</c>) → <c>expires_at</c> (<c>token_expired</c>) → owner's <c>last_app_ok_at</c> older than
/// 30 days (<c>reauth_required</c>) → principal (<see cref="KeysynthAuth.UserIdClaim"/>, <see cref="KeysynthAuth.DeviceIdClaim"/>).
/// Requests without a Bearer header are NoResult (the cookie scheme may still authenticate them).
/// </summary>
public sealed class DeviceTokenHandler(
    IOptionsMonitor<AuthenticationSchemeOptions> options,
    ILoggerFactory loggerFactory,
    UrlEncoder encoder,
    IDeviceRepository devices,
    IClock clock)
    : AuthenticationHandler<AuthenticationSchemeOptions>(options, loggerFactory, encoder)
{
    public const string TokenInvalid = "token_invalid";
    public const string TokenExpired = "token_expired";
    public const string ReauthRequired = "reauth_required";

    protected override async Task<AuthenticateResult> HandleAuthenticateAsync()
    {
        var headers = Request.Headers.Authorization;
        if (headers.Count == 0)
            return AuthenticateResult.NoResult();
        if (headers.Count > 1)
            return Fail(TokenInvalid);

        var header = headers[0] ?? "";
        const string bearer = "Bearer ";
        if (!header.StartsWith(bearer, StringComparison.OrdinalIgnoreCase))
            return AuthenticateResult.NoResult();

        var token = header[bearer.Length..].Trim();
        if (!DeviceTokens.IsWellFormed(token))
            return Fail(TokenInvalid);

        var hash = DeviceTokens.Hash(token);
        var device = await devices.FindByTokenHashAsync(hash, Context.RequestAborted);
        if (device is null || !CryptographicOperations.FixedTimeEquals(device.TokenHash, hash))
            return Fail(TokenInvalid);

        var now = clock.UtcNow;
        if (now >= device.ExpiresAt)
            return Fail(TokenExpired);
        if (now - device.LastAppOkAt > Limits.AppOkMaxAge)
            return Fail(ReauthRequired);

        if (device.LastSeenAt is not { } seen || now - seen >= Limits.LastSeenWriteInterval)
            await devices.TouchAsync(device.Id, now, Context.RequestAborted);

        var identity = new ClaimsIdentity(
        [
            new Claim(KeysynthAuth.UserIdClaim, device.UserId.ToString(CultureInfo.InvariantCulture)),
            new Claim(KeysynthAuth.DeviceIdClaim, device.DeviceId),
        ], KeysynthAuth.DeviceScheme);
        return AuthenticateResult.Success(new AuthenticationTicket(new ClaimsPrincipal(identity), Scheme.Name));
    }

    private AuthenticateResult Fail(string code)
    {
        Context.Items[KeysynthAuth.DeviceErrorItem] = code;
        return AuthenticateResult.Fail(code);
    }
}
