using System.Text;
using System.Text.Json;
using Keysynth.Api.Auth;
using Keysynth.Api.Http;
using Keysynth.Core;

namespace Keysynth.Api.Endpoints;

public sealed record PresetMeta(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted)
{
    public static PresetMeta Of(PresetRow r) => new(r.Slug, r.Revision, r.UpdatedAt, r.Deleted);
}

public sealed record PresetDto(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted, JsonElement? Patch);

/// <summary>Presets (cloud-api.md §3.3): device-only, revision ETags, If-Match / If-None-Match: *, tombstones.</summary>
public static class PresetEndpoints
{
    public static void MapPresetEndpoints(this IEndpointRouteBuilder app)
    {
        var g = app.MapGroup("/api/presets").RequireAuthorization(KeysynthAuth.DevicePolicy);
        g.MapGet("", List);
        g.MapGet("/{slug}", Get);
        g.MapPut("/{slug}", Put).RequireRateLimiting(RateLimits.PresetWrites);
        g.MapDelete("/{slug}", Delete).RequireRateLimiting(RateLimits.PresetWrites);
    }

    private static long UserId(HttpContext ctx) => KeysynthAuth.DevicePrincipal(ctx.User)!.Value.UserId;

    private static IResult WithETag(HttpContext ctx, PresetRow row, object body, int status)
    {
        ctx.Response.Headers.ETag = ETags.ForRevision(row.Revision);
        return Results.Json(body, statusCode: status);
    }

    private static IResult Mismatch(PresetRow? current) =>
        Results.Json(new { error = "revision_mismatch", current = current is null ? null : PresetMeta.Of(current) }, statusCode: 412);

    /// <summary>
    /// <c>If-Match</c> → revision to match: <c>"&lt;n&gt;"</c>, or <c>*</c> = the current revision of an existing row
    /// (RFC 9110). Anything else (weak tags, lists) never matches.
    /// </summary>
    private static async Task<uint?> ExpectedRevisionAsync(string header, long userId, string slug, IPresetRepository presets, HttpContext ctx)
    {
        if (header.Trim() == "*")
            return (await presets.GetAsync(userId, slug, ctx.RequestAborted))?.Revision;
        return ETags.ParseRevision(header);
    }

    private static async Task<IResult> List(HttpContext ctx, IPresetRepository presets)
    {
        var rows = await presets.ListAsync(UserId(ctx), ctx.RequestAborted);
        return Results.Ok(new ItemsDto<PresetMeta>(rows.Select(PresetMeta.Of).ToList()));
    }

    private static async Task<IResult> Get(HttpContext ctx, string slug, IPresetRepository presets)
    {
        if (!Slugs.IsValid(slug))
            return Errors.InvalidSlug;
        if (await presets.GetAsync(UserId(ctx), slug, ctx.RequestAborted) is not { } row)
            return Errors.NotFound;

        JsonElement? patch = null;
        if (row.Patch is not null)
        {
            using var doc = JsonDocument.Parse(row.Patch);
            patch = doc.RootElement.Clone();
        }
        return WithETag(ctx, row, new PresetDto(row.Slug, row.Revision, row.UpdatedAt, row.Deleted, patch), 200);
    }

    private static async Task<IResult> Put(HttpContext ctx, string slug, IPresetRepository presets, IClock clock)
    {
        if (!Slugs.IsValid(slug))
            return Errors.InvalidSlug;

        var ifMatch = ctx.Request.Headers.IfMatch;
        var ifNoneMatch = ctx.Request.Headers.IfNoneMatch;
        if (ifMatch.Count == 0 && ifNoneMatch.Count == 0)
            return Errors.PreconditionRequired;
        if (ifMatch.Count > 0 && ifNoneMatch.Count > 0)
            return Errors.InvalidRequest;
        if (ifNoneMatch.Count > 0 && ifNoneMatch.ToString().Trim() != "*")
            return Errors.InvalidRequest;

        var (doc, error) = await Bodies.ReadJsonAsync(ctx.Request, Limits.MaxRequestBodyBytes);
        if (error is not null)
            return error;
        string patch;
        using (doc)
        {
            var root = doc!.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                return Errors.InvalidRequest;
            if (!root.TryGetProperty("patch", out var p) || p.ValueKind != JsonValueKind.Object
                || !p.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number)
                return Errors.InvalidPatch;
            patch = p.GetRawText();
        }
        if (Encoding.UTF8.GetByteCount(patch) > Limits.MaxPatchBytes)
            return Errors.PayloadTooLarge;

        var userId = UserId(ctx);
        var now = clock.UtcNow;
        PresetWriteResult result;
        var created = ifNoneMatch.Count > 0;
        if (created)
        {
            result = await presets.CreateAsync(userId, slug, patch, now, Limits.MaxPresetsPerUser, ctx.RequestAborted);
        }
        else
        {
            if (await ExpectedRevisionAsync(ifMatch.ToString(), userId, slug, presets, ctx) is not { } expected)
                return Mismatch(await presets.GetAsync(userId, slug, ctx.RequestAborted));
            result = await presets.UpdateAsync(userId, slug, expected, patch, now, ctx.RequestAborted);
        }

        return result.Status switch
        {
            PresetWriteStatus.Ok => WithETag(ctx, result.Row!, PresetMeta.Of(result.Row!), created ? 201 : 200),
            PresetWriteStatus.TooManyPresets => Errors.Of(409, "too_many_presets"),
            _ => Mismatch(result.Row),
        };
    }

    private static async Task<IResult> Delete(HttpContext ctx, string slug, IPresetRepository presets, IClock clock)
    {
        if (!Slugs.IsValid(slug))
            return Errors.InvalidSlug;
        var ifMatch = ctx.Request.Headers.IfMatch;
        if (ifMatch.Count == 0)
            return Errors.PreconditionRequired;

        var userId = UserId(ctx);
        if (await ExpectedRevisionAsync(ifMatch.ToString(), userId, slug, presets, ctx) is not { } expected)
        {
            var current = await presets.GetAsync(userId, slug, ctx.RequestAborted);
            return current is null ? Errors.NotFound : Mismatch(current);
        }

        var result = await presets.DeleteAsync(userId, slug, expected, clock.UtcNow, ctx.RequestAborted);
        return result.Status switch
        {
            PresetWriteStatus.Ok => WithETag(ctx, result.Row!, PresetMeta.Of(result.Row!), 200),
            PresetWriteStatus.NotFound => Errors.NotFound,
            _ => Mismatch(result.Row),
        };
    }
}
