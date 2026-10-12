using System.Globalization;
using System.Text.Json;
using Keysynth.Api.Auth;
using Keysynth.Api.Http;
using Keysynth.Core;

namespace Keysynth.Api.Endpoints;

public sealed record DeviceMinted(string DeviceId, string Token, DateTime ExpiresAt, string AccountId, string AccountName);
public sealed record DeviceDto(string DeviceId, string Name, DateTime CreatedAt, DateTime? LastSeenAt, DateTime ExpiresAt);
public sealed record DeviceMe(string DeviceId, string Name, string AccountId, string AccountName, DateTime ExpiresAt);
public sealed record ItemsDto<T>(IReadOnlyList<T> Items);

/// <summary>Devices (cloud-api.md §3.1). POST/GET/DELETE by id: cookie only; /me: device only.</summary>
public static class DeviceEndpoints
{
    public static void MapDeviceEndpoints(this IEndpointRouteBuilder app)
    {
        var g = app.MapGroup("/api/devices");

        g.MapPost("", Mint).RequireAuthorization(KeysynthAuth.CookiePolicy).RequireRateLimiting(RateLimits.DeviceMint);
        g.MapGet("", List).RequireAuthorization(KeysynthAuth.CookiePolicy);
        // "me" is not a UUID v4, so it never collides with {deviceId}; the literal route wins anyway.
        g.MapGet("/me", Me).RequireAuthorization(KeysynthAuth.DevicePolicy);
        g.MapDelete("/me", DeleteMe).RequireAuthorization(KeysynthAuth.DevicePolicy);
        g.MapDelete("/{deviceId}", Delete).RequireAuthorization(KeysynthAuth.CookiePolicy);
    }

    private static string AccountId(long id) => id.ToString(CultureInfo.InvariantCulture);

    private static async Task<IResult> Mint(HttpContext ctx, IDeviceRepository devices, IClock clock)
    {
        var me = KeysynthAuth.GrantedCookieUser(ctx.User)!;
        var userId = KeysynthAuth.RequireCookieUserId(ctx);

        var (doc, error) = await Bodies.ReadJsonAsync(ctx.Request, Limits.MaxRequestBodyBytes);
        if (error is not null)
            return error;
        string deviceId, name;
        using (doc)
        {
            var root = doc!.RootElement;
            if (root.ValueKind != JsonValueKind.Object
                || !root.TryGetProperty("deviceId", out var idEl) || idEl.ValueKind != JsonValueKind.String
                || !root.TryGetProperty("name", out var nameEl) || nameEl.ValueKind != JsonValueKind.String)
                return Errors.InvalidRequest;
            deviceId = idEl.GetString()!;
            name = nameEl.GetString()!.Trim();
        }
        if (!DeviceIds.IsValid(deviceId) || name.Length is 0 or > Limits.MaxDeviceNameLength || name.Any(char.IsControl))
            return Errors.InvalidRequest;

        var token = DeviceTokens.Generate();
        var now = clock.UtcNow;
        var expiresAt = now + Limits.DeviceTokenLifetime;
        var outcome = await devices.MintAsync(
            new DeviceMint(userId, deviceId, name, DeviceTokens.Hash(token), now, expiresAt), Limits.MaxDevicesPerUser, ctx.RequestAborted);
        if (outcome == MintOutcome.TooManyDevices)
            return Errors.Of(409, "too_many_devices");

        var accountName = KeysynthAuth.Truncate(me.Name, Limits.MaxUserNameLength);
        var body = new DeviceMinted(deviceId, token, expiresAt, AccountId(userId), accountName);
        ctx.Response.Headers.CacheControl = "no-store";
        return Results.Json(body, statusCode: outcome == MintOutcome.Created ? 201 : 200);
    }

    private static async Task<IResult> List(HttpContext ctx, IDeviceRepository devices)
    {
        var rows = await devices.ListAsync(KeysynthAuth.RequireCookieUserId(ctx), ctx.RequestAborted);
        return Results.Ok(new ItemsDto<DeviceDto>(rows.Select(d => new DeviceDto(d.DeviceId, d.Name, d.CreatedAt, d.LastSeenAt, d.ExpiresAt)).ToList()));
    }

    private static async Task<IResult> Delete(HttpContext ctx, string deviceId, IDeviceRepository devices)
    {
        if (!DeviceIds.IsValid(deviceId))
            return Errors.NotFound;
        return await devices.DeleteAsync(KeysynthAuth.RequireCookieUserId(ctx), deviceId, ctx.RequestAborted)
            ? Results.NoContent()
            : Errors.NotFound;
    }

    private static async Task<IResult> Me(HttpContext ctx, IDeviceRepository devices, IUserRepository users)
    {
        var (userId, deviceId) = KeysynthAuth.DevicePrincipal(ctx.User)!.Value;
        var device = await devices.GetAsync(userId, deviceId, ctx.RequestAborted);
        var user = await users.GetAsync(userId, ctx.RequestAborted);
        if (device is null || user is null)
        {
            // deleted between auth and here
            ctx.Response.Headers.WWWAuthenticate = $"Bearer error=\"{DeviceTokenHandler.TokenInvalid}\"";
            return Errors.Of(401, DeviceTokenHandler.TokenInvalid);
        }
        return Results.Ok(new DeviceMe(device.DeviceId, device.Name, AccountId(userId), user.Name, device.ExpiresAt));
    }

    private static async Task<IResult> DeleteMe(HttpContext ctx, IDeviceRepository devices)
    {
        var (userId, deviceId) = KeysynthAuth.DevicePrincipal(ctx.User)!.Value;
        await devices.DeleteAsync(userId, deviceId, ctx.RequestAborted);
        return Results.NoContent();
    }
}
