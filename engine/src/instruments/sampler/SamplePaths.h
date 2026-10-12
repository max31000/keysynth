#pragma once
// Resolution of sample-library paths in `sampler` state (`{"sfz": "assets/samples/<lib>/<entry>.sfz"}`).
//
// Search order (docs/PRESETS.md, ARCHITECTURE §11):
//   1. the path as given: absolute, or relative to the repo root (AppPaths). Must lie inside an allow-listed root
//      (presets/, userdata/, assets/, plugins/).
//   2. for paths below `assets/` that do not exist there: `$KS_ASSETS_DIR/<rest>` (KS_ASSETS_DIR points at a
//      directory laid out like the repo's `assets/`, e.g. the main checkout's `M:/Projects/Piano/assets`).
//   3. when the repo root is a git worktree under `<main>/.claude/worktrees/<name>`: `<main>/assets/<rest>`.
//   4. for `assets/samples/<lib>/<path>`: the sample cache `<cache>/<lib>/<path>` (ARCHITECTURE §15; default
//      AppPaths::discover().sampleCache, setSampleCacheDir() overrides).
// A fallback candidate must lie inside its own assets directory (no `..` escapes).

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ks {

// Sample cache root searched by resolveSamplePath (thread-safe).
void setSampleCacheDir(const std::filesystem::path& dir);
std::filesystem::path sampleCacheDir();

// Auto-fetch of a missing `assets/samples/<entry id>` (PROTOCOL.md "Auto-fetch on patch load"). The hook is set by
// the sample service (message thread); the sampler asks it from prepare() (fresh = true) and resourcesChanged().
enum class SampleFetchState {
    Unavailable, // unknown entry (not in the manifest, local-only id): normal missing-sfz error
    Waiting,     // a download job covers it (or the manifest is being fetched): stay silent with `loading` = 1
    Failed,      // its download failed and was reported (samples_error): error state, no extra `log`
    NotLinked,   // a downloadable entry, but the engine is not linked to an account: friendly notice, no error
    Offline      // linked, but the catalog is unreachable: friendly notice; retried, resourcesChanged() when back
};
using SampleFetchHook = std::function<SampleFetchState(const std::string& entryId, bool fresh)>;
void setSampleFetchHook(SampleFetchHook hook);
SampleFetchState querySampleFetch(const std::string& entryId, bool fresh);
// "assets/samples/<lib>/<path>" -> "<lib>/<path>"; nullopt for other specs.
std::optional<std::string> sampleEntryIdOf(const std::string& sfzSpec);

struct SamplePathResult {
    std::optional<std::filesystem::path> path; // existing file, or nullopt
    std::string error;                         // why not (when path is empty)
};

// `root` = repo root (AppPaths::root). Control/loader thread only (touches the filesystem).
SamplePathResult resolveSamplePath(const std::string& spec, const std::filesystem::path& root);

// Directories searched for `assets/...` (existing ones, in order): <root>/assets, $KS_ASSETS_DIR, <main>/assets.
std::vector<std::filesystem::path> sampleAssetDirs(const std::filesystem::path& root);

} // namespace ks
