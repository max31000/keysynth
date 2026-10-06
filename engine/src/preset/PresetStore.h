#pragma once
// PresetStore (control thread): factory presets presets/factory/** (read-only) + user presets userdata/presets.

#include "core/AppPaths.h"
#include "core/Patch.h"

#include <string>
#include <vector>

namespace ks {

struct PresetInfo {
    std::string path; // relative to repo root, '/' separators
    std::string name;
    std::string category;
    std::vector<std::string> tags;
    bool factory = false;
};

class PresetStore {
public:
    explicit PresetStore(AppPaths paths) : paths_(std::move(paths)) {}
    const AppPaths& paths() const noexcept { return paths_; }

    std::vector<PresetInfo> list() const;
    // Throws PatchError: bad_path (outside allow-list), not_found, parse_error.
    Patch load(const std::string& path, std::vector<std::string>* warnings = nullptr) const;
    // Saves to userdata/presets/<slug>.json. Throws PatchError("exists") when !overwrite and the file exists.
    // Returns the path relative to the repo root.
    std::string save(const Patch& patch, const std::string& name, const std::string& category, bool overwrite) const;

    static std::string slugify(const std::string& name);
    static Patch loadFile(const std::string& absolutePath, std::vector<std::string>* warnings = nullptr);

private:
    AppPaths paths_;
};

} // namespace ks
