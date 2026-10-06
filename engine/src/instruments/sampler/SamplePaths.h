#pragma once
// Resolution of sample-library paths in `sampler` state (`{"sfz": "assets/samples/<lib>/<entry>.sfz"}`).
//
// Search order (docs/PRESETS.md, ARCHITECTURE §11):
//   1. the path as given: absolute, or relative to the repo root (AppPaths). Must lie inside an allow-listed root
//      (presets/, userdata/, assets/, plugins/).
//   2. for paths below `assets/` that do not exist there: `$KS_ASSETS_DIR/<rest>` (KS_ASSETS_DIR points at a
//      directory laid out like the repo's `assets/`, e.g. the main checkout's `M:/Projects/Piano/assets`).
//   3. when the repo root is a git worktree under `<main>/.claude/worktrees/<name>`: `<main>/assets/<rest>`.
// A fallback candidate must lie inside its own assets directory (no `..` escapes).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ks {

struct SamplePathResult {
    std::optional<std::filesystem::path> path; // existing file, or nullopt
    std::string error;                         // why not (when path is empty)
};

// `root` = repo root (AppPaths::root). Control/loader thread only (touches the filesystem).
SamplePathResult resolveSamplePath(const std::string& spec, const std::filesystem::path& root);

// Directories searched for `assets/...` (existing ones, in order): <root>/assets, $KS_ASSETS_DIR, <main>/assets.
std::vector<std::filesystem::path> sampleAssetDirs(const std::filesystem::path& root);

} // namespace ks
