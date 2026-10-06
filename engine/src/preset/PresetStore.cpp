#include "preset/PresetStore.h"

#include "core/PatchModel.h"
#include "preset/PatchJson.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ks {

namespace {

nlohmann::json readJsonFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw PatchError("not_found", "cannot open " + pathToUtf8(p));
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        std::string what = e.what();
        if (what.size() > 160) what = what.substr(0, 160) + "...";
        throw PatchError("parse_error", pathToUtf8(p.filename()) + ": " + what);
    }
}

void scanDir(const fs::path& dir, bool factory, const AppPaths& paths, std::vector<PresetInfo>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        PresetInfo info;
        info.path = paths.relativeToRoot(it->path());
        info.factory = factory;
        try {
            const auto j = readJsonFile(it->path());
            const auto meta = j.value("meta", nlohmann::json::object());
            info.name = meta.value("name", pathToUtf8(it->path().stem()));
            info.category = meta.value("category", std::string());
            if (auto t = meta.find("tags"); t != meta.end() && t->is_array())
                for (const auto& tag : *t)
                    if (tag.is_string()) info.tags.push_back(tag.get<std::string>());
        } catch (...) {
            continue; // unreadable presets are skipped in listings
        }
        out.push_back(std::move(info));
    }
}

} // namespace

std::vector<PresetInfo> PresetStore::list() const {
    std::vector<PresetInfo> out;
    scanDir(paths_.factoryPresets, true, paths_, out);
    scanDir(paths_.userPresets, false, paths_, out);
    std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) {
        if (a.factory != b.factory) return a.factory;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });
    return out;
}

Patch PresetStore::loadFile(const std::string& absolutePath, std::vector<std::string>* warnings) {
    return patchFromJson(readJsonFile(pathFromUtf8(absolutePath)), warnings);
}

Patch PresetStore::load(const std::string& path, std::vector<std::string>* warnings) const {
    auto resolved = paths_.resolveAllowed(path);
    if (!resolved) throw PatchError("bad_path", "path is outside the allowed roots: " + path);
    if (resolved->extension() != ".json") throw PatchError("bad_path", "presets must be .json files");
    return patchFromJson(readJsonFile(*resolved), warnings);
}

std::string PresetStore::slugify(const std::string& name) {
    std::string s;
    bool dash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            s.push_back(static_cast<char>(std::tolower(c)));
            dash = false;
        } else if (!s.empty() && !dash) {
            s.push_back('-');
            dash = true;
        }
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    if (s.empty()) s = "preset";
    if (s.size() > 64) s.resize(64);
    return s;
}

std::string PresetStore::save(const Patch& patchIn, const std::string& name, const std::string& category,
                              bool overwrite) const {
    if (name.empty()) throw PatchError("bad_request", "preset name is empty");
    Patch patch = patchIn;
    patch.meta.name = name;
    if (!category.empty()) patch.meta.category = category;
    if (patch.meta.author.empty() || patch.meta.author == "factory") patch.meta.author = "user";
    std::error_code ec;
    fs::create_directories(paths_.userPresets, ec);
    const fs::path file = paths_.userPresets / (slugify(name) + ".json");
    if (!overwrite && fs::exists(file, ec)) throw PatchError("exists", "preset already exists: " + pathToUtf8(file));
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw PatchError("io_error", "cannot write " + pathToUtf8(tmp));
        f << patchToJson(patch).dump(2) << '\n';
        if (!f) throw PatchError("io_error", "write failed: " + pathToUtf8(tmp));
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
        if (ec) throw PatchError("io_error", "cannot move " + pathToUtf8(tmp) + ": " + ec.message());
    }
    return paths_.relativeToRoot(file);
}

} // namespace ks
