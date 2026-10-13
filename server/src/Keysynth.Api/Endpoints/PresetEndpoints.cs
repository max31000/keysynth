using System.Text.Json;
using Keysynth.Core;

namespace Keysynth.Api.Endpoints;

public sealed record PresetDto(string Slug, uint Revision, DateTime UpdatedAt, bool Deleted, JsonElement? Patch);

/// <summary>Presets (cloud-api.md §3.3); behaviour in <see cref="DocumentEndpoints"/>.</summary>
public static class PresetEndpoints
{
    public static readonly DocumentKind Kind = new(
        "/api/presets", "patch", "invalid_patch", "too_many_presets", Limits.MaxPresetsPerUser, Limits.MaxPatchBytes,
        (r, patch) => new PresetDto(r.Slug, r.Revision, r.UpdatedAt, r.Deleted, patch));

    public static void MapPresetEndpoints(this IEndpointRouteBuilder app) => app.MapDocumentEndpoints<IPresetRepository>(Kind);
}
