using Keysynth.Core;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace Keysynth.Infrastructure;

public sealed class KeysynthOptions
{
    /// <summary>Directory holding <c>index.json</c> and <c>&lt;lib&gt;/…</c> (container: the read-only volume).</summary>
    public string SamplesRoot { get; set; } = "/data/samples";
}

public interface ISampleIndexProvider
{
    /// <summary>The current index; null when <c>index.json</c> is missing or invalid (→ 503 index_unavailable).</summary>
    SampleIndex? Current { get; }
}

/// <summary>
/// Loads <c>&lt;SamplesRoot&gt;/index.json</c> and reloads it when its mtime or size changes (checked on every call:
/// one stat per request). A failed load is cached per (mtime, size) so a broken file is not re-parsed per request.
/// </summary>
public sealed class SampleIndexProvider(IOptions<KeysynthOptions> options, ILogger<SampleIndexProvider> logger) : ISampleIndexProvider
{
    private sealed record State(DateTime MTime, long Size, SampleIndex? Index);

    private readonly object _gate = new();
    private volatile State? _state;

    public string IndexPath => Path.Combine(options.Value.SamplesRoot, "index.json");

    public SampleIndex? Current
    {
        get
        {
            var path = IndexPath;
            DateTime mtime;
            long size;
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists)
                {
                    _state = null;
                    return null;
                }
                mtime = info.LastWriteTimeUtc;
                size = info.Length;
            }
            catch (IOException ex)
            {
                logger.LogWarning(ex, "Cannot stat sample index {Path}", path);
                return null;
            }

            var state = _state;
            if (state is not null && state.MTime == mtime && state.Size == size)
                return state.Index;

            lock (_gate)
            {
                state = _state;
                if (state is not null && state.MTime == mtime && state.Size == size)
                    return state.Index;

                SampleIndex? index = null;
                try
                {
                    index = SampleIndex.Parse(File.ReadAllBytes(path));
                    if (index.SkippedPaths > 0)
                        logger.LogWarning("Sample index {Path}: {Count} invalid file paths ignored", path, index.SkippedPaths);
                    logger.LogInformation("Sample index loaded: {Path}, etag {ETag}", path, index.ETag);
                }
                catch (Exception ex) when (ex is FormatException or IOException or UnauthorizedAccessException)
                {
                    logger.LogError(ex, "Sample index {Path} is invalid", path);
                }
                _state = new State(mtime, size, index);
                return index;
            }
        }
    }
}
