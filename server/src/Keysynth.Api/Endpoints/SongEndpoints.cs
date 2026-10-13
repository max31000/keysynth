using System.Text.Json;
using Keysynth.Core;

namespace Keysynth.Api.Endpoints;

public sealed record SongDto(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted, JsonElement? Song);

/// <summary>Songs / setlists (cloud-api.md §3.3a); behaviour in <see cref="DocumentEndpoints"/>.</summary>
public static class SongEndpoints
{
    public static readonly DocumentKind Kind = new(
        "/api/songs", "song", "invalid_song", "too_many_songs", Limits.MaxSongsPerUser, Limits.MaxSongBytes,
        (r, song) => new SongDto(r.Slug, r.Revision, r.UpdatedAt, r.Deleted, song));

    public static void MapSongEndpoints(this IEndpointRouteBuilder app) => app.MapDocumentEndpoints<ISongRepository>(Kind);
}
