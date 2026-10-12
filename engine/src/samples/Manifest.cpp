#include "samples/Manifest.h"

#include "samples/SamplePathRules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

using nlohmann::json;

namespace ks::samples {

namespace {
std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

uint64_t u64(const json& j, const char* k) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return 0;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        return v > 0 ? static_cast<uint64_t>(v) : 0;
    }
    const double d = it->get<double>();
    return d > 0 && d < 1.8e19 ? static_cast<uint64_t>(d) : 0;
}

bool isSha(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
} // namespace

std::optional<Manifest> Manifest::parse(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    if (auto f = j.find("format"); f == j.end() || !f->is_number() || f->get<double>() != 1.0) return std::nullopt;
    Manifest m;
    std::set<std::string> groupIds;
    if (auto g = j.find("groups"); g != j.end() && g->is_array())
        for (const auto& gj : *g) {
            if (!gj.is_object()) continue;
            ManifestGroup grp{str(gj, "id"), str(gj, "name")};
            if (grp.id.empty() || !groupIds.insert(grp.id).second) continue;
            if (grp.name.empty()) grp.name = grp.id;
            m.groups.push_back(std::move(grp));
        }
    std::set<std::string> libIds;
    if (auto ls = j.find("libraries"); ls != j.end() && ls->is_array())
        for (const auto& lj : *ls) {
            if (!lj.is_object()) continue;
            ManifestLibrary lib;
            lib.id = str(lj, "id");
            if (!isValidLibraryId(lib.id) || !libIds.insert(lib.id).second) continue;
            lib.name = str(lj, "name");
            lib.group = str(lj, "group");
            if (!groupIds.count(lib.group)) continue;
            lib.licence = str(lj, "licence");
            lib.author = str(lj, "author");
            lib.attribution = str(lj, "attribution");
            if (auto fs = lj.find("files"); fs != lj.end() && fs->is_array())
                for (const auto& fj : *fs) {
                    ManifestFile f;
                    if (fj.is_object()) {
                        f.path = str(fj, "path");
                        f.bytes = u64(fj, "bytes");
                        f.sha256 = str(fj, "sha256");
                    }
                    f.valid = isValidManifestPath(f.path) && isSha(f.sha256);
                    lib.files.push_back(std::move(f));
                }
            {
                // Names that collide on NTFS (case-insensitive) or with our own temp suffixes are invalid.
                std::set<std::string> folded;
                for (auto& f : lib.files) {
                    std::string k = f.path;
                    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    const bool tempName = k.size() >= 5 && (k.ends_with(".part") || k.ends_with(".tmp"));
                    if (tempName || !folded.insert(k).second) f.valid = false;
                }
            }
            uint64_t total = 0;
            for (const auto& f : lib.files) total += f.bytes;
            lib.bytes = total;
            if (auto es = lj.find("entries"); es != lj.end() && es->is_array())
                for (const auto& ej : *es) {
                    if (!ej.is_object()) continue;
                    ManifestEntry e;
                    e.sfz = str(ej, "sfz");
                    if (!isValidManifestPath(e.sfz)) continue;
                    e.id = lib.id + "/" + e.sfz;
                    e.name = str(ej, "name");
                    if (e.name.empty()) e.name = e.sfz;
                    std::set<size_t> seen;
                    bool ok = true;
                    if (auto fi = ej.find("files"); fi != ej.end() && fi->is_array())
                        for (const auto& ix : *fi) {
                            if (!ix.is_number_unsigned() && !ix.is_number_integer()) {
                                ok = false;
                                break;
                            }
                            const int64_t i = ix.get<int64_t>();
                            if (i < 0 || static_cast<size_t>(i) >= lib.files.size()) {
                                ok = false;
                                break;
                            }
                            if (seen.insert(static_cast<size_t>(i)).second) e.files.push_back(static_cast<size_t>(i));
                        }
                    if (!ok) continue;
                    for (size_t i : e.files) e.bytes += lib.files[i].bytes;
                    lib.entries.push_back(std::move(e));
                }
            m.libraries.push_back(std::move(lib));
        }
    return m;
}

const ManifestLibrary* Manifest::library(const std::string& id) const {
    for (const auto& l : libraries)
        if (l.id == id) return &l;
    return nullptr;
}

std::optional<std::pair<const ManifestLibrary*, const ManifestEntry*>> Manifest::entry(const std::string& entryId) const {
    const auto split = splitEntryId(entryId);
    if (!split) return std::nullopt;
    const ManifestLibrary* l = library(split->first);
    if (!l) return std::nullopt;
    for (const auto& e : l->entries)
        if (e.id == entryId) return std::make_pair(l, &e);
    return std::nullopt;
}

} // namespace ks::samples
