using System.Text.RegularExpressions;

namespace Keysynth.Core;

/// <summary>Preset slugs (cloud-api.md §5.1).</summary>
public static partial class Slugs
{
    public const int MaxLength = 64;

    [GeneratedRegex("^[a-z0-9]+(-[a-z0-9]+)*$", RegexOptions.CultureInvariant)]
    private static partial Regex Pattern();

    public static bool IsValid(string? slug) =>
        slug is { Length: > 0 and <= MaxLength } && Pattern().IsMatch(slug);
}

/// <summary>Engine device ids: UUID v4, lowercase, with dashes (cloud-api.md §2.2).</summary>
public static partial class DeviceIds
{
    [GeneratedRegex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", RegexOptions.CultureInvariant)]
    private static partial Regex Pattern();

    public static bool IsValid(string? id) => id is { Length: 36 } && Pattern().IsMatch(id);
}

/// <summary>Sample library ids and file paths (cloud-api.md §4.1; identical rules in builder, API and engine).</summary>
public static partial class SamplePaths
{
    public const int MaxPathLength = 240;
    public const int MaxLibraryIdLength = 64;

    [GeneratedRegex("^[a-z0-9][a-z0-9-]*$", RegexOptions.CultureInvariant)]
    private static partial Regex LibraryIdPattern();

    private static readonly HashSet<string> Reserved = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    public static bool IsValidLibraryId(string? id) =>
        id is { Length: > 0 and <= MaxLibraryIdLength } && LibraryIdPattern().IsMatch(id);

    /// <summary>
    /// Relative '/'-separated path: non-empty segments, no '.'/'..', no leading '/', no '\\', ':' or control chars,
    /// no segment ending in '.' or ' ', no Windows reserved device names (with any extension), ≤ 240 chars.
    /// </summary>
    public static bool IsValidPath(string? path)
    {
        if (string.IsNullOrEmpty(path) || path.Length > MaxPathLength)
            return false;

        for (var i = 0; i < path.Length; i++)
        {
            var c = path[i];
            if (c < 0x20 || c == 0x7F || c == '\\' || c == ':')
                return false;
            if (char.IsHighSurrogate(c))
            {
                if (i + 1 >= path.Length || !char.IsLowSurrogate(path[i + 1]))
                    return false;
                i++;
            }
            else if (char.IsLowSurrogate(c))
            {
                return false;
            }
        }

        foreach (var segment in path.Split('/'))
        {
            if (segment.Length == 0 || segment[^1] == '.' || segment[^1] == ' ')
                return false; // also covers leading '/', '//', '.', '..'
            var dot = segment.IndexOf('.');
            var stem = dot < 0 ? segment : segment[..dot];
            if (Reserved.Contains(stem.TrimEnd(' ')))
                return false;
        }
        return true;
    }
}
