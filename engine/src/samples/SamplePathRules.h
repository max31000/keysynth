#pragma once
// Manifest path rules (cloud-api spec §4.1, design §11): identical for the index builder, the API and the engine.
// A manifest path is relative to its library dir, '/'-separated, and must pass isValidManifestPath() before the
// engine creates any file for it; cachePathFor() additionally canonicalises and checks containment.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ks::samples {

// [a-z0-9][a-z0-9-]*, <= 64 chars.
bool isValidLibraryId(std::string_view id);
// Non-empty segments, no '.'/'..', no leading '/', no '\\', ':', control chars (< 0x20, 0x7F), no segment ending
// in '.' or ' ', no Windows reserved device name (CON PRN AUX NUL COM1-9 LPT1-9, with any extension, any case),
// valid UTF-8, total <= 240 bytes. The engine also rejects `< > " | ? *` (no such file can exist on Windows).
bool isValidManifestPath(std::string_view path);
// <cacheRoot>/<lib>/<path> when lib/path are valid and the canonical result stays below <cacheRoot>/<lib>.
std::optional<std::filesystem::path> cachePathFor(const std::filesystem::path& cacheRoot, std::string_view lib,
                                                  std::string_view path);
// Entry id "<lib>/<sfz path>" -> {lib, sfz path}; nullopt when malformed.
std::optional<std::pair<std::string, std::string>> splitEntryId(std::string_view entryId);

} // namespace ks::samples
