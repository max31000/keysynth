#pragma once
// Song (setlist entry) data model + JSON (ARCHITECTURE §18, schema docs/PRESETS.md "Songs"). Control thread only.
// Lenient: unknown keys (incl. the reserved notation/backing fields) are kept verbatim in `extra` and written back.

#include "core/AppPaths.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ks {

inline constexpr int kSongFormat = 1;

// Section switching from the keyboard (MIDI notes; -1 = none).
struct ControlKeys {
    bool enabled = false;
    int prev = 21; // A0
    int next = 22; // A#0
    bool operator==(const ControlKeys&) const = default;
};
nlohmann::json controlKeysToJson(const ControlKeys& k);
// Applies the keys present in `j` onto `base` (nulls = none). Throws PatchError("bad_request") on bad values.
ControlKeys controlKeysFromJson(const nlohmann::json& j, ControlKeys base);

struct SongSection {
    std::string name;
    std::optional<std::string> preset;  // logical path or bare user preset slug
    std::optional<std::string> pattern; // logical pattern path, "" = empty pattern
    std::optional<bool> drums;          // true: drums on + play; false: stop
    std::optional<double> tempo;
    std::string notes;
    nlohmann::json extra = nlohmann::json::object(); // unknown / reserved keys (bars, start_bar, ...)
};

struct Song {
    int format = kSongFormat;
    std::string id; // file slug (the file name wins)
    std::string title, artist, key, notes;
    std::optional<double> tempo;
    std::optional<std::pair<int, int>> timeSig;
    std::optional<ControlKeys> controlKeys;
    std::vector<SongSection> sections;
    nlohmann::json extra = nlohmann::json::object(); // unknown / reserved keys (notation, backing, ...)
};

inline constexpr size_t kMaxSongSections = 64;

Song songFromJson(const nlohmann::json& j, std::vector<std::string>* warnings = nullptr);
nlohmann::json songToJson(const Song& s);
// "" when the song may be saved, else the reason (title, sections, preset/pattern refs, ranges).
std::string validateSong(const Song& s);
// A section `preset` value -> logical preset path ("" when it is neither a logical preset path nor a slug).
std::string presetRefToPath(const std::string& ref);
bool isPatternRef(const std::string& ref); // "" or a logical pattern path

struct SongInfo {
    std::string path, id, title, artist;
    int sections = 0;
    bool factory = false;
};

// Factory (presets/songs) + user (userdata/songs) song files. Stateless; paths come from the caller's AppPaths.
// Throws PatchError: bad_path, not_found, parse_error, exists, io_error, read_only.
namespace songs {
std::vector<SongInfo> list(const AppPaths& paths);
Song load(const AppPaths& paths, const std::string& path, std::vector<std::string>* warnings = nullptr);
// Writes userdata/songs/<id>.json (id: song.id if a valid slug, else the slugified title). Returns the logical path.
std::string save(const AppPaths& paths, Song song, bool overwrite);
void remove(const AppPaths& paths, const std::string& path);
} // namespace songs

} // namespace ks
