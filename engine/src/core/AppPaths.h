#pragma once
// Repository / user-data locations and the path allow-list (ARCHITECTURE §11).
// Every path that arrives in a request is resolved relative to the repo root and must lie inside one of the
// allow-listed roots: presets/, userdata/, assets/, plugins/.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ks {

struct AppPaths {
    std::filesystem::path root;          // repo root (contains presets/factory)
    std::filesystem::path factoryPresets; // root/presets/factory (read-only)
    std::filesystem::path userdata;      // root/userdata (gitignored)
    std::filesystem::path userPresets;   // root/userdata/presets
    std::filesystem::path settingsFile;  // root/userdata/settings.json
    std::filesystem::path uiDist;        // root/ui/dist

    static AppPaths fromRoot(const std::filesystem::path& root);
    // Search order: $KS_ROOT, ancestors of `exeDir`, ancestors of the cwd, compile-time KS_SOURCE_DIR.
    static AppPaths discover(const std::filesystem::path& exeDir = {});

    std::vector<std::filesystem::path> allowedRoots() const;
    // Resolve `p` (absolute, or relative to root) and check the allow-list. nullopt if outside.
    std::optional<std::filesystem::path> resolveAllowed(const std::string& p) const;
    // Path relative to root with '/' separators (for protocol replies); absolute if outside root.
    std::string relativeToRoot(const std::filesystem::path& p) const;
};

// UTF-8 <-> path helpers ('/' separators on output).
std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path pathFromUtf8(const std::string& s);

bool isPathInside(const std::filesystem::path& p, const std::filesystem::path& root);
// Same, without touching the filesystem.
bool isPathInsideLexical(const std::filesystem::path& p, const std::filesystem::path& root);

} // namespace ks
