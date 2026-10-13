using System.Text;
using System.Text.Json;
using Keysynth.Api.Auth;
using Keysynth.Api.Http;
using Keysynth.Core;

namespace Keysynth.Api.Endpoints;

/// <summary><c>PresetMeta</c> / <c>SongMeta</c> (cloud-api.md §3.3, §3.3a): identical wire shape.</summary>
public sealed record DocumentMeta(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted)
{
    public static DocumentMeta Of(DocumentRow r) => new(r.Slug, r.Revision, r.UpdatedAt, r.Deleted);
}

/// <summary>What differs between the revisioned document resources (presets, songs).</summary>
/// <param name="Route">Group route, e.g. <c>/api/presets</c>.</param>
/// <param name="BodyKey">Property of the PUT body / GET response holding the document (<c>patch</c>, <c>song</c>).</param>
/// <param name="InvalidCode">400 code for a body value that is not an object with numeric <c>format</c>.</param>
/// <param name="TooManyCode">409 code at <paramref name="MaxPerUser"/> rows (tombstones count).</param>
/// <param name="MaxBytes">Limit of the serialised document (413 above).</param>
/// <param name="Dto">Full resource (meta + document, null when deleted) for <c>GET /{slug}</c>.</param>
public sealed record DocumentKind(
    string Route, string BodyKey, string InvalidCode, string TooManyCode, int MaxPerUser, int MaxBytes,
    Func<DocumentRow, JsonElement?, object> Dto);

/// <summary>
/// Revisioned per-user documents (cloud-api.md §3.3, shared by §3.3a): device-only, revision ETags,
/// If-Match / If-None-Match: *, tombstones. Writes use the one <see cref="RateLimits.PresetWrites"/> bucket.
/// </summary>
public static class DocumentEndpoints
{
    public static void MapDocumentEndpoints<TRepo>(this IEndpointRouteBuilder app, DocumentKind kind)
        where TRepo : IDocumentRepository
    {
        // Handlers resolve TRepo themselves: generic service parameters on route-handler lambdas crash the
        // RouteHandlerAnalyzer (AD0001) in the .NET 8 SDK.
        static IDocumentRepository Repo(HttpContext ctx) => ctx.RequestServices.GetRequiredService<TRepo>();
        static IClock Clock(HttpContext ctx) => ctx.RequestServices.GetRequiredService<IClock>();

        var g = app.MapGroup(kind.Route).RequireAuthorization(KeysynthAuth.DevicePolicy);
        g.MapGet("", (Func<HttpContext, Task<IResult>>)(ctx => List(ctx, Repo(ctx))));
        g.MapGet("/{slug}", (HttpContext ctx, string slug) => Get(ctx, slug, Repo(ctx), kind));
        g.MapPut("/{slug}", (HttpContext ctx, string slug) => Put(ctx, slug, Repo(ctx), Clock(ctx), kind))
            .RequireRateLimiting(RateLimits.PresetWrites);
        g.MapDelete("/{slug}", (HttpContext ctx, string slug) => Delete(ctx, slug, Repo(ctx), Clock(ctx)))
            .RequireRateLimiting(RateLimits.PresetWrites);
    }

    private static long UserId(HttpContext ctx) => KeysynthAuth.DevicePrincipal(ctx.User)!.Value.UserId;

    private static IResult WithETag(HttpContext ctx, DocumentRow row, object body, int status)
    {
        ctx.Response.Headers.ETag = ETags.ForRevision(row.Revision);
        return Results.Json(body, statusCode: status);
    }

    private static IResult Mismatch(DocumentRow? current) =>
        Results.Json(new { error = "revision_mismatch", current = current is null ? null : DocumentMeta.Of(current) }, statusCode: 412);

    /// <summary>
    /// <c>If-Match</c> → revision to match: <c>"&lt;n&gt;"</c>, or <c>*</c> = the current revision of an existing row
    /// (RFC 9110). Anything else (weak tags, lists) never matches.
    /// </summary>
    private static async Task<uint?> ExpectedRevisionAsync(string header, long userId, string slug, IDocumentRepository repo, HttpContext ctx)
    {
        if (header.Trim() == "*")
            return (await repo.GetAsync(userId, slug, ctx.RequestAborted))?.Revision;
        return ETags.ParseRevision(header);
    }

    private static async Task<IResult> List(HttpContext ctx, IDocumentRepository repo)
    {
        var rows = await repo.ListAsync(UserId(ctx), ctx.RequestAborted);
        return Results.Ok(new ItemsDto<DocumentMeta>(rows.Select(DocumentMeta.Of).ToList()));
    }

    private static async Task<IResult> Get(HttpContext ctx, string slug, IDocumentRepository repo, DocumentKind kind)
    {
        if (!Slugs.IsValid(slug))
            return Errors.InvalidSlug;
        if (await repo.GetAsync(UserId(ctx), slug, ctx.RequestAborted) is not { } row)
            return Errors.NotFound;

        JsonElement? body = null;
        if (row.Body is not null)
        {
            using var doc = JsonDocument.Parse(row.Body);
            body = doc.RootElement.Clone();
        }
        return WithETag(ctx, row, kind.Dto(row, body), 200);
    }

    private static async Task<IResult> Put(HttpContext ctx, string slug, IDocumentRepository repo, IClock clock, DocumentKind kind)
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
        string body;
        using (doc)
        {
            var root = doc!.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                return Errors.InvalidRequest;
            if (!root.TryGetProperty(kind.BodyKey, out var p) || p.ValueKind != JsonValueKind.Object
                || !p.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number)
                return Errors.Of(400, kind.InvalidCode);
            body = p.GetRawText();
        }
        if (Encoding.UTF8.GetByteCount(body) > kind.MaxBytes)
            return Errors.PayloadTooLarge;

        var userId = UserId(ctx);
        var now = clock.UtcNow;
        DocumentWriteResult result;
        var created = ifNoneMatch.Count > 0;
        if (created)
        {
            result = await repo.CreateAsync(userId, slug, body, now, kind.MaxPerUser, ctx.RequestAborted);
        }
        else
        {
            if (await ExpectedRevisionAsync(ifMatch.ToString(), userId, slug, repo, ctx) is not { } expected)
                return Mismatch(await repo.GetAsync(userId, slug, ctx.RequestAborted));
            result = await repo.UpdateAsync(userId, slug, expected, body, now, ctx.RequestAborted);
        }

        return result.Status switch
        {
            DocumentWriteStatus.Ok => WithETag(ctx, result.Row!, DocumentMeta.Of(result.Row!), created ? 201 : 200),
            DocumentWriteStatus.TooMany => Errors.Of(409, kind.TooManyCode),
            _ => Mismatch(result.Row),
        };
    }

    private static async Task<IResult> Delete(HttpContext ctx, string slug, IDocumentRepository repo, IClock clock)
    {
        if (!Slugs.IsValid(slug))
            return Errors.InvalidSlug;
        var ifMatch = ctx.Request.Headers.IfMatch;
        if (ifMatch.Count == 0)
            return Errors.PreconditionRequired;

        var userId = UserId(ctx);
        if (await ExpectedRevisionAsync(ifMatch.ToString(), userId, slug, repo, ctx) is not { } expected)
        {
            var current = await repo.GetAsync(userId, slug, ctx.RequestAborted);
            return current is null ? Errors.NotFound : Mismatch(current);
        }

        var result = await repo.DeleteAsync(userId, slug, expected, clock.UtcNow, ctx.RequestAborted);
        return result.Status switch
        {
            DocumentWriteStatus.Ok => WithETag(ctx, result.Row!, DocumentMeta.Of(result.Row!), 200),
            DocumentWriteStatus.NotFound => Errors.NotFound,
            _ => Mismatch(result.Row),
        };
    }
}
