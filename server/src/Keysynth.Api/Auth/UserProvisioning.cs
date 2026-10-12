using System.Collections.Concurrent;
using Keysynth.Core;
using Mvv42.Auth.Client;

namespace Keysynth.Api.Auth;

/// <summary>Per-process memo of the last users upsert per Telegram id (bounded by the invite-only user base).</summary>
public sealed class ProvisioningCache
{
    internal sealed record Entry(long UserId, string AuthUserId, string Name, DateTime WrittenAt);

    internal ConcurrentDictionary<long, Entry> Entries { get; } = new();
}

/// <summary>
/// Local user of a granted cookie request (cloud-api.md §2.1, §2.3): provisioned by <c>tg</c>, <c>name</c> kept in
/// step with the token, <c>last_app_ok_at = now</c> — the DB write happens at most once per hour per user (or at
/// once when sub/name changed).
/// </summary>
public sealed class UserProvisioning(IUserRepository users, ProvisioningCache cache, IClock clock)
{
    public async Task<long> EnsureAsync(Mvv42User me, CancellationToken ct)
    {
        var now = clock.UtcNow;
        var name = me.Name.Length > Limits.MaxUserNameLength ? me.Name[..Limits.MaxUserNameLength] : me.Name;
        if (cache.Entries.TryGetValue(me.TelegramId, out var e)
            && e.AuthUserId == me.AuthUserId && e.Name == name
            && now - e.WrittenAt < Limits.AppOkWriteInterval && now >= e.WrittenAt)
            return e.UserId;

        var id = await users.UpsertAsync(me.TelegramId, me.AuthUserId, name, now, ct);
        cache.Entries[me.TelegramId] = new ProvisioningCache.Entry(id, me.AuthUserId, name, now);
        return id;
    }
}

/// <summary>Runs after authorization: every request that passed with a granted mvv42 cookie gets its local user.</summary>
public sealed class UserProvisioningMiddleware(RequestDelegate next)
{
    public async Task InvokeAsync(HttpContext context, UserProvisioning provisioning)
    {
        if (KeysynthAuth.GrantedCookieUser(context.User) is { } me)
            context.Items[KeysynthAuth.UserIdItem] = await provisioning.EnsureAsync(me, context.RequestAborted);
        await next(context);
    }
}
