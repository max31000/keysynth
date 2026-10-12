using Keysynth.Api.Auth;
using Keysynth.Api.Http;
using Keysynth.Core;
using Keysynth.Infrastructure;
using Microsoft.Extensions.Options;

namespace Keysynth.Api.Endpoints;

/// <summary>Samples (cloud-api.md §3.2): manifest = index.json verbatim; files authorised here, served by nginx.</summary>
public static class SampleEndpoints
{
    /// <summary>Env/config flag: serve files from <c>Keysynth:SamplesRoot</c> directly (local dev without nginx).</summary>
    public const string DevServeFilesKey = "KS_DEV_SERVE_FILES";

    public static void MapSampleEndpoints(this IEndpointRouteBuilder app)
    {
        var g = app.MapGroup("/api/samples").RequireAuthorization(KeysynthAuth.AnyUserPolicy);
        g.MapGet("/manifest", Manifest).RequireRateLimiting(RateLimits.Manifest);
        g.MapGet("/files/{lib}/{**path}", File);
    }

    private static IResult IndexUnavailable() => Errors.Of(503, "index_unavailable");

    private static IResult Manifest(HttpContext ctx, ISampleIndexProvider provider)
    {
        if (provider.Current is not { } index)
            return IndexUnavailable();

        ctx.Response.Headers.ETag = index.ETag;
        ctx.Response.Headers.CacheControl = "no-cache";
        if (ETags.NoneMatchHits(ctx.Request.Headers.IfNoneMatch.ToString(), index.ETag))
            return Results.StatusCode(StatusCodes.Status304NotModified);
        return Results.Bytes(index.Bytes, "application/json");
    }

    private static IResult File(
        HttpContext ctx, string lib, string? path, ISampleIndexProvider provider, IConfiguration config,
        IOptions<KeysynthOptions> options, IHostEnvironment env)
    {
        // Route values arrive URL-decoded. Validate before the lookup (defence in depth: the index itself only holds
        // valid paths), then require exact membership in the loaded index.
        if (!SamplePaths.IsValidLibraryId(lib) || !SamplePaths.IsValidPath(path))
            return Errors.NotFound;
        if (provider.Current is not { } index)
            return IndexUnavailable();
        if (!index.Contains(lib, path!))
            return Errors.NotFound;

        ctx.Response.Headers.CacheControl = "private, no-store";

        // Dev only (never in Production: it would bypass nginx limits). Testing = the integration tests.
        if (config[DevServeFilesKey] == "1" && (env.IsDevelopment() || env.IsEnvironment("Testing")))
        {
            var root = Path.GetFullPath(options.Value.SamplesRoot);
            var full = Path.GetFullPath(Path.Combine(root, lib, path!.Replace('/', Path.DirectorySeparatorChar)));
            if (!full.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.Ordinal)
                || !System.IO.File.Exists(full))
                return Errors.NotFound;
            // ETag like nginx's (mtime-size) so If-Range resume behaves the same as in production.
            var info = new FileInfo(full);
            var etag = new Microsoft.Net.Http.Headers.EntityTagHeaderValue(
                $"\"{new DateTimeOffset(info.LastWriteTimeUtc).ToUnixTimeSeconds():x}-{info.Length:x}\"");
            return TypedResults.PhysicalFile(full, "application/octet-stream", lastModified: info.LastWriteTimeUtc,
                entityTag: etag, enableRangeProcessing: true);
        }

        // nginx (internal location /keysynth/_files/) serves the bytes with Range/If-Range/ETag. No Content-Type here:
        // nginx would keep an upstream one instead of its own.
        ctx.Response.Headers["X-Accel-Redirect"] = AccelPath.For(lib, path!);
        return Results.Empty;
    }
}
