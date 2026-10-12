#pragma once
// Sample catalogue = server index.json (cloud-api spec §4.2). Parsed leniently: libraries with an invalid id or
// group reference are dropped; files keep a `valid` flag (path rules, SamplePathRules.h) so a job touching an
// invalid one fails with `bad_path` instead of writing anything.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ks::samples {

struct ManifestFile {
    std::string path; // relative to the library dir
    uint64_t bytes = 0;
    std::string sha256; // 64 lowercase hex
    bool valid = false; // path + sha256 pass validation
};

struct ManifestEntry {
    std::string id;   // "<lib>/<sfz>"
    std::string sfz;  // relative to the library dir
    std::string name;
    std::vector<size_t> files; // indices into the library's files
    uint64_t bytes = 0;        // sum of its unique files
};

struct ManifestLibrary {
    std::string id, name, group, licence, author, attribution;
    uint64_t bytes = 0;
    std::vector<ManifestFile> files;
    std::vector<ManifestEntry> entries;
};

struct ManifestGroup {
    std::string id, name;
};

struct Manifest {
    std::vector<ManifestGroup> groups;
    std::vector<ManifestLibrary> libraries;

    // nullopt when the document is not a format-1 manifest object.
    static std::optional<Manifest> parse(const std::string& jsonText);

    const ManifestLibrary* library(const std::string& id) const;
    // {library, entry} for an entry id.
    std::optional<std::pair<const ManifestLibrary*, const ManifestEntry*>> entry(const std::string& entryId) const;
};

// Cache key of a file: "<lib>/<path>".
inline std::string fileKey(const std::string& lib, const std::string& path) { return lib + "/" + path; }

} // namespace ks::samples
