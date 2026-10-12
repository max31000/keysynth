#include "samples/CacheIndex.h"

#include "core/AppPaths.h"
#include "samples/SamplePathRules.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ks::samples {

const char* toString(EntryStatus s) {
    switch (s) {
    case EntryStatus::Local: return "local";
    case EntryStatus::Cached: return "cached";
    case EntryStatus::Partial: return "partial";
    case EntryStatus::Missing: return "missing";
    }
    return "missing";
}

std::string nowIso8601() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

namespace {
std::optional<fs::path> pathOfKey(const fs::path& root, const std::string& key) {
    const auto split = splitEntryId(key); // same shape: "<lib>/<path>"
    if (!split) return std::nullopt;
    return cachePathFor(root, split->first, split->second);
}
} // namespace

void CacheIndex::load(const fs::path& root) {
    *this = CacheIndex{};
    std::ifstream f(root / "cache.json", std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return;
    if (auto e = j.find("manifestEtag"); e != j.end() && e->is_string()) manifestEtag = e->get<std::string>();
    if (auto e = j.find("manifestFetchedAt"); e != j.end() && e->is_string()) manifestFetchedAt = e->get<std::string>();
    std::error_code ec;
    if (auto fj = j.find("files"); fj != j.end() && fj->is_object())
        for (const auto& [key, v] : fj->items()) {
            if (!v.is_object()) continue;
            const auto p = pathOfKey(root, key);
            if (!p) continue;
            CachedFile c;
            c.bytes = v.value("bytes", uint64_t{0});
            c.sha256 = v.value("sha256", std::string());
            c.verifiedAt = v.value("verifiedAt", std::string());
            const auto size = fs::file_size(*p, ec);
            if (ec || size != c.bytes) continue;
            files[key] = std::move(c);
        }
    if (auto pj = j.find("partial"); pj != j.end() && pj->is_object())
        for (const auto& [key, v] : pj->items()) {
            if (!v.is_object()) continue;
            const auto p = pathOfKey(root, key);
            if (!p) continue;
            fs::path part = *p;
            part += ".part";
            const auto size = fs::file_size(part, ec);
            if (ec) continue;
            PartialFile pf;
            pf.bytesDone = size;
            pf.etag = v.value("etag", std::string());
            partial[key] = std::move(pf);
        }
}

bool CacheIndex::save(const fs::path& root) const {
    json fj = json::object(), pj = json::object();
    for (const auto& [k, c] : files) fj[k] = {{"bytes", c.bytes}, {"sha256", c.sha256}, {"verifiedAt", c.verifiedAt}};
    for (const auto& [k, p] : partial) pj[k] = {{"bytesDone", p.bytesDone}, {"etag", p.etag}};
    const json j = {{"format", 1},
                    {"manifestEtag", manifestEtag.empty() ? json(nullptr) : json(manifestEtag)},
                    {"manifestFetchedAt", manifestFetchedAt.empty() ? json(nullptr) : json(manifestFetchedAt)},
                    {"files", fj},
                    {"partial", pj}};
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path tmp = root / "cache.json.tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << j.dump(1) << '\n';
        if (!o) return false;
    }
    fs::rename(tmp, root / "cache.json", ec);
    return !ec;
}

bool CacheIndex::isCached(const std::string& lib, const ManifestFile& f) const {
    auto it = files.find(fileKey(lib, f.path));
    return it != files.end() && it->second.bytes == f.bytes && it->second.sha256 == f.sha256;
}

EntryStatus CacheIndex::entryStatus(const ManifestLibrary& lib, const ManifestEntry& e, uint64_t* cachedBytes) const {
    size_t have = 0;
    uint64_t bytes = 0;
    for (size_t i : e.files) {
        const ManifestFile& f = lib.files[i];
        if (isCached(lib.id, f)) {
            ++have;
            bytes += f.bytes;
        }
    }
    if (cachedBytes) *cachedBytes = bytes;
    if (!e.files.empty() && have == e.files.size()) return EntryStatus::Cached;
    return have == 0 ? EntryStatus::Missing : EntryStatus::Partial;
}

} // namespace ks::samples
