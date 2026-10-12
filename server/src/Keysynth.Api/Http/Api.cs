using System.Globalization;
using System.Text.Json;
using System.Threading.RateLimiting;
using Keysynth.Api.Auth;
using Keysynth.Core;
using Microsoft.AspNetCore.RateLimiting;

namespace Keysynth.Api.Http;

/// <summary><c>{"error":"&lt;code&gt;"}</c> responses (cloud-api.md §1).</summary>
public static class Errors
{
    public static IResult Of(int status, string code) => Results.Json(new { error = code }, statusCode: status);

    public static IResult InvalidRequest => Of(400, "invalid_request");
    public static IResult InvalidSlug => Of(400, "invalid_slug");
    public static IResult InvalidPatch => Of(400, "invalid_patch");
    public static IResult NotFound => Of(404, "not_found");
    public static IResult PayloadTooLarge => Of(413, "payload_too_large");
    public static IResult PreconditionRequired => Of(428, "precondition_required");

    /// <summary>Codes for bodiless 4xx/5xx produced by the framework (unknown route, wrong method, …).</summary>
    public static string ForStatus(int status) => status switch
    {
        400 => "invalid_request",
        401 => "unauthorized",
        403 => "forbidden",
        404 or 405 => "not_found",
        413 => "payload_too_large",
        429 => "rate_limited",
        _ when status >= 500 => "internal",
        _ => "invalid_request",
    };
}

public static class Bodies
{
    /// <summary>
    /// Reads at most <paramref name="max"/> bytes of the body and parses it as JSON. Errors: 413 over the limit,
    /// 400 <c>invalid_request</c> for malformed JSON.
    /// </summary>
    public static async Task<(JsonDocument? Doc, IResult? Error)> ReadJsonAsync(HttpRequest request, int max)
    {
        if (request.ContentLength > max)
            return (null, Errors.PayloadTooLarge);

        using var buffer = new MemoryStream();
        var chunk = new byte[16 * 1024];
        int read;
        while ((read = await request.Body.ReadAsync(chunk, request.HttpContext.RequestAborted)) > 0)
        {
            if (buffer.Length + read > max)
                return (null, Errors.PayloadTooLarge);
            buffer.Write(chunk, 0, read);
        }

        try
        {
            return (JsonDocument.Parse(buffer.ToArray()), null);
        }
        catch (JsonException)
        {
            return (null, Errors.InvalidRequest);
        }
    }
}

public static class ETags
{
    /// <summary>
    /// Weak comparison (RFC 9110 §13.1.2) of an <c>If-None-Match</c> header against <paramref name="etag"/>; true for
    /// <c>*</c>.
    /// </summary>
    public static bool NoneMatchHits(string? header, string etag)
    {
        if (string.IsNullOrWhiteSpace(header))
            return false;
        foreach (var raw in header.Split(','))
        {
            var tag = raw.Trim();
            if (tag == "*")
                return true;
            if (tag.StartsWith("W/", StringComparison.Ordinal))
                tag = tag[2..];
            if (tag == etag)
                return true;
        }
        return false;
    }

    public static string ForRevision(uint revision) => "\"" + revision.ToString(CultureInfo.InvariantCulture) + "\"";

    /// <summary>Single strong ETag <c>"&lt;digits&gt;"</c> → revision; anything else → null (never matches).</summary>
    public static uint? ParseRevision(string header)
    {
        var tag = header.Trim();
        if (tag.Length < 3 || tag[0] != '"' || tag[^1] != '"')
            return null;
        var digits = tag[1..^1];
        if (digits.Length > 10 || !digits.All(char.IsAsciiDigit))
            return null;
        return uint.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out var rev) ? rev : null;
    }
}

/// <summary>Per-user fixed windows (cloud-api.md §7); rejected → 429 <c>rate_limited</c> + <c>Retry-After</c>.</summary>
public static class RateLimits
{
    public const string DeviceMint = "device-mint";
    public const string PresetWrites = "preset-writes";
    public const string Manifest = "manifest";

    public static IServiceCollection AddKeysynthRateLimits(this IServiceCollection services)
    {
        services.AddRateLimiter(o =>
        {
            o.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
            o.AddPolicy(DeviceMint, ctx => PerUser(ctx, Limits.DeviceMintPerHour, TimeSpan.FromHours(1)));
            o.AddPolicy(PresetWrites, ctx => PerUser(ctx, Limits.PresetWritesPerMinute, TimeSpan.FromMinutes(1)));
            o.AddPolicy(Manifest, ctx => PerUser(ctx, Limits.ManifestPerMinute, TimeSpan.FromMinutes(1)));
            o.OnRejected = async (ctx, ct) =>
            {
                var retry = ctx.Lease.TryGetMetadata(MetadataName.RetryAfter, out var after)
                    ? Math.Max(1, (int)Math.Ceiling(after.TotalSeconds))
                    : 60;
                ctx.HttpContext.Response.Headers.RetryAfter = retry.ToString(CultureInfo.InvariantCulture);
                await ctx.HttpContext.Response.WriteAsJsonAsync(new { error = "rate_limited" }, ct);
            };
        });
        return services;
    }

    private static RateLimitPartition<string> PerUser(HttpContext ctx, int permits, TimeSpan window)
    {
        // Runs after authorization + provisioning: every limited endpoint has a local user.
        var key = KeysynthAuth.UserId(ctx)?.ToString(CultureInfo.InvariantCulture)
                  ?? "ip:" + ctx.Connection.RemoteIpAddress;
        return RateLimitPartition.GetFixedWindowLimiter(key, _ => new FixedWindowRateLimiterOptions
        {
            PermitLimit = permits,
            Window = window,
            QueueLimit = 0,
            AutoReplenishment = true,
        });
    }
}
