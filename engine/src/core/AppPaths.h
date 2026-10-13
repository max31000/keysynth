#pragma once
// App / user-data locations and the path allow-list (ARCHITECTURE §11, §15; layout table: cloud-api spec §6.1).
//
// Mode: installed iff `installed.flag` exists next to the exe (app dir = exe dir, data root
// %LOCALAPPDATA%\keysynth), else dev (repo checkout: data root = <repo>/userdata, the layout used so far).
// KS_DATA_DIR replaces the data root in both modes (discover() only; fromRoot() is always the dev layout).
//
// Paths on the wire are *logical* (PROTOCOL.md "Sessions, Origin, Host"): `userdata/...` = the active profile,
// `presets/...` = the app's factory presets/patterns, `assets/...` = repo assets (the sampler also searches
// KS_ASSETS_DIR and the sample cache), `plugins/...` = the user plugins dir. resolveAllowed() maps a logical (or
// absolute) path to disk and enforces the allow-list; relativeToRoot() maps a disk path back to its logical form.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ks {

struct AppPaths {
    enum class Mode { Dev, Installed };
    Mode mode = Mode::Dev;

    std::filesystem::path root;           // app dir (read-only content): repo root (dev) / install dir (installed)
    std::filesystem::path factoryPresets; // root/presets/factory
    std::filesystem::path factoryPatterns; // root/presets/patterns
    std::filesystem::path factorySongs;   // root/presets/songs (ARCHITECTURE §18)
    std::filesystem::path uiDist;         // root/ui/dist

    std::filesystem::path dataRoot;       // D: <repo>/userdata (dev), %LOCALAPPDATA%/keysynth (installed)
    std::filesystem::path settingsFile;   // D/settings.json
    std::filesystem::path cloudFile;      // D/cloud.json
    std::filesystem::path logsDir;        // D/logs
    std::filesystem::path pluginsDir;     // root/plugins (dev), D/plugins (installed)
    std::filesystem::path pluginBuildDir; // root/plugins/.build (dev), D/faust-cache (installed)
    std::filesystem::path sampleCache;    // D/samples (both modes; dev D = <repo>/userdata)

    std::filesystem::path localProfile;   // profile root: <repo>/userdata (dev), D/userdata (installed)
    std::filesystem::path userdata;       // active profile: localProfile or localProfile/accounts/<id>
    std::filesystem::path userPresets;    // userdata/presets
    std::filesystem::path userPatterns;   // userdata/patterns
    std::filesystem::path userSongs;      // userdata/songs
    std::filesystem::path recordingsDir;  // localProfile/recordings
    std::string accountId;                // active profile's account ("" = local)

    bool installed() const noexcept { return mode == Mode::Installed; }

    // Dev layout for a repo root (tests, --root).
    static AppPaths fromRoot(const std::filesystem::path& root);
    // Installed layout: app dir + data root.
    static AppPaths installedLayout(const std::filesystem::path& appDir, const std::filesystem::path& dataRoot);
    // Mode detection from `exeDir` (default: the running executable's dir). Dev search order for the repo root:
    // $KS_ROOT, ancestors of exeDir, ancestors of the cwd, compile-time KS_SOURCE_DIR. KS_DATA_DIR applies.
    static AppPaths discover(const std::filesystem::path& exeDir = {});

    // Switch the active profile ("" = local). accountId must be a plain id ([A-Za-z0-9_-]{1,64}); else ignored.
    bool setProfile(const std::string& accountId);
    std::filesystem::path accountProfileDir(const std::string& accountId) const;
    static bool isValidAccountId(const std::string& id);

    std::vector<std::filesystem::path> allowedRoots() const;
    // Resolve `p` (logical, relative to root, or absolute) and check the allow-list. nullopt if outside.
    std::optional<std::filesystem::path> resolveAllowed(const std::string& p) const;
    // Logical path of a disk path ('/' separators); the absolute path when it has no logical form.
    std::string relativeToRoot(const std::filesystem::path& p) const;
};

// UTF-8 <-> path helpers ('/' separators on output).
std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path pathFromUtf8(const std::string& s);

bool isPathInside(const std::filesystem::path& p, const std::filesystem::path& root);
// Same, without touching the filesystem.
bool isPathInsideLexical(const std::filesystem::path& p, const std::filesystem::path& root);

} // namespace ks
