using System.Security.Cryptography;
using System.Text.Json;

namespace Keysynth.Core;

/// <summary>
/// Parsed server <c>index.json</c> (cloud-api.md §4.2): the verbatim bytes (= manifest), its ETag and the set of
/// servable files per library. Immutable.
/// </summary>
public sealed class SampleIndex
{
    private readonly Dictionary<string, HashSet<string>> _files;

    private SampleIndex(byte[] bytes, string etag, Dictionary<string, HashSet<string>> files, int skipped)
    {
        Bytes = bytes;
        ETag = etag;
        _files = files;
        SkippedPaths = skipped;
    }

    public byte[] Bytes { get; }

    /// <summary><c>"&lt;first 16 hex of SHA-256(index.json)&gt;"</c>, quotes included.</summary>
    public string ETag { get; }

    /// <summary>Paths dropped because they fail <see cref="SamplePaths.IsValidPath"/> (never served).</summary>
    public int SkippedPaths { get; }

    /// <summary>Exact, case-sensitive membership of <paramref name="path"/> in <c>files[]</c> of library <paramref name="lib"/>.</summary>
    public bool Contains(string lib, string path) =>
        _files.TryGetValue(lib, out var set) && set.Contains(path);

    /// <exception cref="FormatException">Not a valid index (structure, ids, JSON).</exception>
    public static SampleIndex Parse(byte[] bytes)
    {
        var files = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
        var skipped = 0;
        try
        {
            using var doc = JsonDocument.Parse(bytes);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object
                || !root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.Number
                || !root.TryGetProperty("libraries", out var libraries) || libraries.ValueKind != JsonValueKind.Array)
                throw new FormatException("index.json: expected an object with numeric 'format' and array 'libraries'");

            foreach (var lib in libraries.EnumerateArray())
            {
                if (lib.ValueKind != JsonValueKind.Object
                    || !lib.TryGetProperty("id", out var idEl) || idEl.ValueKind != JsonValueKind.String
                    || !lib.TryGetProperty("files", out var filesEl) || filesEl.ValueKind != JsonValueKind.Array)
                    throw new FormatException("index.json: library without string 'id' / array 'files'");
                var id = idEl.GetString()!;
                if (!SamplePaths.IsValidLibraryId(id) || files.ContainsKey(id))
                    throw new FormatException("index.json: invalid or duplicate library id");

                var set = new HashSet<string>(StringComparer.Ordinal);
                foreach (var file in filesEl.EnumerateArray())
                {
                    if (file.ValueKind != JsonValueKind.Object
                        || !file.TryGetProperty("path", out var pathEl) || pathEl.ValueKind != JsonValueKind.String)
                        throw new FormatException("index.json: file without string 'path'");
                    var path = pathEl.GetString()!;
                    if (SamplePaths.IsValidPath(path))
                        set.Add(path);
                    else
                        skipped++;
                }
                files[id] = set;
            }
        }
        catch (JsonException ex)
        {
            throw new FormatException("index.json: invalid JSON", ex);
        }

        var hash = SHA256.HashData(bytes);
        var etag = "\"" + Convert.ToHexString(hash, 0, 8).ToLowerInvariant() + "\"";
        return new SampleIndex(bytes, etag, files, skipped);
    }
}
