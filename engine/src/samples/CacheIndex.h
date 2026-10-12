#pragma once
// samples/cache.json (cloud-api spec §4.3): verified files + resumable partial downloads. Plain data; SampleService
// guards it with its mutex and is its only writer.

#include "samples/Manifest.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ks::samples {

struct CachedFile {
    uint64_t bytes = 0;
    std::string sha256, verifiedAt;
};

struct PartialFile {
    uint64_t bytesDone = 0;
    std::string etag;
};

enum class EntryStatus { Local, Cached, Partial, Missing };
const char* toString(EntryStatus s);

struct CacheIndex {
    std::string manifestEtag, manifestFetchedAt;
    std::map<std::string, CachedFile> files;    // key "<lib>/<path>"
    std::map<std::string, PartialFile> partial; // key "<lib>/<path>" (file <cache>/<lib>/<path>.part)

    // Loads cache.json and drops entries whose file is missing / has another size, partials without a .part.
    void load(const std::filesystem::path& cacheRoot);
    bool save(const std::filesystem::path& cacheRoot) const; // temp + rename

    // A file counts as cached when listed with the manifest's size and sha256.
    bool isCached(const std::string& lib, const ManifestFile& f) const;
    // Status from the cache only (Local is decided by the caller). cachedBytes = bytes of cached unique files.
    EntryStatus entryStatus(const ManifestLibrary& lib, const ManifestEntry& e, uint64_t* cachedBytes = nullptr) const;
};

std::string nowIso8601(); // "2026-10-12T14:03:00Z"

} // namespace ks::samples
