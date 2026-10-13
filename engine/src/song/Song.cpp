#include "song/Song.h"

#include "cloud/SyncService.h" // isValidSlug
#include "control/Trust.h"     // isLogicalPath
#include "core/PatchModel.h"   // PatchError
#include "preset/PresetStore.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ks {

namespace {

void warn(std::vector<std::string>* w, const std::string& m) {
    if (w) w->push_back(m);
}

std::optional<std::string> optStr(const json& j, const char* k, std::vector<std::string>* w, const std::string& where) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (it->is_string()) return it->get<std::string>();
    warn(w, where + "." + k + ": expected a string, ignored");
    return std::nullopt;
}

std::optional<double> optNum(const json& j, const char* k, std::vector<std::string>* w, const std::string& where) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (it->is_number() && std::isfinite(it->get<double>())) return it->get<double>();
    warn(w, where + "." + k + ": expected a number, ignored");
    return std::nullopt;
}

std::optional<bool> optBool(const json& j, const char* k, std::vector<std::string>* w, const std::string& where) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (it->is_boolean()) return it->get<bool>();
    warn(w, where + "." + k + ": expected a boolean, ignored");
    return std::nullopt;
}

json extraOf(const json& j, std::initializer_list<const char*> known) {
    json e = json::object();
    std::set<std::string> k(known.begin(), known.end());
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!k.count(it.key())) e[it.key()] = it.value();
    return e;
}

int noteOrNone(const json& v) {
    if (v.is_null()) return -1;
    if (!v.is_number()) throw PatchError("bad_request", "control key must be a MIDI note 0..127 or null");
    const double d = v.get<double>();
    if (!(d >= 0 && d <= 127) || d != std::floor(d)) throw PatchError("bad_request", "control key must be 0..127");
    return static_cast<int>(d);
}

// BPM 20..400 or nothing (files are applied without a save-time validation: factory, synced).
std::optional<double> tempoOf(const json& j, std::vector<std::string>* w, const std::string& where) {
    const auto t = optNum(j, "tempo", w, where);
    if (t && !(*t >= 20.0 && *t <= 400.0)) {
        warn(w, where + ".tempo: out of range 20..400, ignored");
        return std::nullopt;
    }
    return t;
}

json noteJson(int n) { return n < 0 ? json(nullptr) : json(n); }

std::string utf8Len(const std::string& s, size_t max, const char* what) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    if (n > max) return std::string(what) + " is longer than " + std::to_string(max) + " characters";
    return {};
}

json readJsonFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw PatchError("not_found", "cannot open " + pathToUtf8(p));
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw PatchError("parse_error", pathToUtf8(p.filename()) + ": invalid song JSON");
    return j;
}

} // namespace

json controlKeysToJson(const ControlKeys& k) {
    return {{"enabled", k.enabled}, {"prev", noteJson(k.prev)}, {"next", noteJson(k.next)}};
}

ControlKeys controlKeysFromJson(const json& j, ControlKeys base) {
    if (!j.is_object()) throw PatchError("bad_request", "control_keys must be an object");
    if (auto it = j.find("enabled"); it != j.end() && !it->is_null()) {
        if (!it->is_boolean()) throw PatchError("bad_request", "control_keys.enabled must be a boolean");
        base.enabled = it->get<bool>();
    }
    if (auto it = j.find("prev"); it != j.end()) base.prev = noteOrNone(*it);
    if (auto it = j.find("next"); it != j.end()) base.next = noteOrNone(*it);
    return base;
}

std::string presetRefToPath(const std::string& ref) {
    if (isLogicalPath(ref, "presets/factory/") || isLogicalPath(ref, "userdata/presets/")) return ref;
    if (cloud::isValidSlug(ref)) return "userdata/presets/" + ref + ".json";
    return {};
}

bool isPatternRef(const std::string& ref) {
    return ref.empty() || isLogicalPath(ref, "presets/patterns/") || isLogicalPath(ref, "userdata/patterns/");
}

Song songFromJson(const json& j, std::vector<std::string>* w) {
    Song s;
    if (!j.is_object()) {
        warn(w, "song: expected an object");
        return s;
    }
    if (auto v = optNum(j, "format", w, "song")) s.format = static_cast<int>(std::clamp(*v, 0.0, 1000.0));
    if (auto v = optStr(j, "id", w, "song")) s.id = *v;
    if (auto v = optStr(j, "title", w, "song")) s.title = *v;
    if (auto v = optStr(j, "artist", w, "song")) s.artist = *v;
    if (auto v = optStr(j, "key", w, "song")) s.key = *v;
    if (auto v = optStr(j, "notes", w, "song")) s.notes = *v;
    s.tempo = tempoOf(j, w, "song");
    if (auto it = j.find("time_sig"); it != j.end() && !it->is_null()) {
        const bool shape = it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number();
        const double n = shape ? (*it)[0].get<double>() : 0.0, d = shape ? (*it)[1].get<double>() : 0.0;
        if (shape && n >= 1 && n <= 16 && n == std::floor(n) && (d == 2 || d == 4 || d == 8 || d == 16))
            s.timeSig = std::make_pair(static_cast<int>(n), static_cast<int>(d));
        else
            warn(w, "song.time_sig: expected [num 1..16, den 2/4/8/16], ignored");
    }
    if (auto it = j.find("control_keys"); it != j.end() && !it->is_null()) {
        try {
            s.controlKeys = controlKeysFromJson(*it, ControlKeys{true, -1, -1});
        } catch (const PatchError& e) {
            warn(w, std::string("song.control_keys: ") + e.what() + ", ignored");
        }
    }
    if (auto it = j.find("sections"); it != j.end() && it->is_array()) {
        int i = 0;
        for (const auto& sj : *it) {
            const std::string where = "sections[" + std::to_string(i++) + "]";
            if (!sj.is_object()) {
                warn(w, where + ": expected an object, ignored");
                continue;
            }
            SongSection sec;
            if (auto v = optStr(sj, "name", w, where)) sec.name = *v;
            sec.preset = optStr(sj, "preset", w, where);
            sec.pattern = optStr(sj, "pattern", w, where);
            sec.drums = optBool(sj, "drums", w, where);
            sec.tempo = tempoOf(sj, w, where);
            if (auto v = optStr(sj, "notes", w, where)) sec.notes = *v;
            sec.extra = extraOf(sj, {"name", "preset", "pattern", "drums", "tempo", "notes"});
            s.sections.push_back(std::move(sec));
        }
    } else if (it != j.end()) {
        warn(w, "song.sections: expected an array");
    }
    s.extra = extraOf(j, {"format", "id", "title", "artist", "key", "notes", "tempo", "time_sig", "control_keys", "sections"});
    return s;
}

json songToJson(const Song& s) {
    json j = s.extra.is_object() ? s.extra : json::object();
    j["format"] = kSongFormat;
    j["id"] = s.id;
    j["title"] = s.title;
    if (!s.artist.empty()) j["artist"] = s.artist;
    if (s.tempo) j["tempo"] = *s.tempo;
    if (s.timeSig) j["time_sig"] = {s.timeSig->first, s.timeSig->second};
    if (!s.key.empty()) j["key"] = s.key;
    if (!s.notes.empty()) j["notes"] = s.notes;
    if (s.controlKeys) j["control_keys"] = controlKeysToJson(*s.controlKeys);
    json secs = json::array();
    for (const auto& sec : s.sections) {
        json o = sec.extra.is_object() ? sec.extra : json::object();
        o["name"] = sec.name;
        if (sec.preset) o["preset"] = *sec.preset;
        if (sec.pattern) o["pattern"] = *sec.pattern;
        if (sec.drums) o["drums"] = *sec.drums;
        if (sec.tempo) o["tempo"] = *sec.tempo;
        if (!sec.notes.empty()) o["notes"] = sec.notes;
        secs.push_back(std::move(o));
    }
    j["sections"] = std::move(secs);
    return j;
}

std::string validateSong(const Song& s) {
    if (s.title.empty()) return "title is empty";
    if (auto e = utf8Len(s.title, 200, "title"); !e.empty()) return e;
    if (auto e = utf8Len(s.artist, 200, "artist"); !e.empty()) return e;
    if (auto e = utf8Len(s.key, 60, "key"); !e.empty()) return e;
    if (auto e = utf8Len(s.notes, 2000, "notes"); !e.empty()) return e;
    if (s.tempo && !(*s.tempo >= 20.0 && *s.tempo <= 400.0)) return "tempo must be 20..400";
    if (s.timeSig) {
        const auto [n, d] = *s.timeSig;
        if (n < 1 || n > 16 || !(d == 2 || d == 4 || d == 8 || d == 16)) return "time_sig: num 1..16, den 2/4/8/16";
    }
    if (s.sections.empty()) return "a song needs at least one section";
    if (s.sections.size() > kMaxSongSections) return "too many sections (max 64)";
    for (size_t i = 0; i < s.sections.size(); ++i) {
        const auto& sec = s.sections[i];
        const std::string where = "sections[" + std::to_string(i) + "]";
        if (sec.name.empty()) return where + ".name is empty";
        if (auto e = utf8Len(sec.name, 60, "name"); !e.empty()) return where + "." + e;
        if (auto e = utf8Len(sec.notes, 2000, "notes"); !e.empty()) return where + "." + e;
        if (sec.preset && presetRefToPath(*sec.preset).empty())
            return where + ".preset must be a path under presets/factory/ or userdata/presets/, or a preset slug";
        if (sec.pattern && !isPatternRef(*sec.pattern))
            return where + ".pattern must be a path under presets/patterns/ or userdata/patterns/";
        if (sec.tempo && !(*sec.tempo >= 20.0 && *sec.tempo <= 400.0)) return where + ".tempo must be 20..400";
    }
    return {};
}

namespace songs {

std::vector<SongInfo> list(const AppPaths& paths) {
    std::vector<SongInfo> out;
    const std::pair<fs::path, bool> dirs[] = {{paths.factorySongs, true}, {paths.userSongs, false}};
    for (const auto& [dir, factory] : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
            try {
                const Song s = songFromJson(readJsonFile(e.path()));
                SongInfo info;
                info.path = paths.relativeToRoot(e.path());
                info.id = pathToUtf8(e.path().stem());
                info.title = s.title.empty() ? info.id : s.title;
                info.artist = s.artist;
                info.sections = static_cast<int>(s.sections.size());
                info.factory = factory;
                out.push_back(std::move(info));
            } catch (const std::exception&) {
                // unreadable songs are skipped
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const SongInfo& a, const SongInfo& b) {
        if (a.factory != b.factory) return a.factory;
        if (a.title != b.title) return a.title < b.title;
        return a.path < b.path;
    });
    return out;
}

Song load(const AppPaths& paths, const std::string& path, std::vector<std::string>* warnings) {
    if (!isLogicalPath(path, "presets/songs/") && !isLogicalPath(path, "userdata/songs/"))
        throw PatchError("bad_path", "songs live under presets/songs/ or userdata/songs/: " + path);
    auto resolved = paths.resolveAllowed(path);
    if (!resolved) throw PatchError("bad_path", "path is outside the allowed roots: " + path);
    if (resolved->extension() != ".json") throw PatchError("bad_path", "songs must be .json files");
    Song s = songFromJson(readJsonFile(*resolved), warnings);
    s.id = pathToUtf8(resolved->stem()); // the file name wins
    return s;
}

std::string save(const AppPaths& paths, Song song, bool overwrite) {
    if (const std::string err = validateSong(song); !err.empty()) throw PatchError("bad_request", err);
    std::string id = cloud::isValidSlug(song.id) ? song.id : PresetStore::slugify(song.title);
    if (!cloud::isValidSlug(id)) id = "song";
    song.id = id;
    std::error_code ec;
    fs::create_directories(paths.userSongs, ec);
    const fs::path file = paths.userSongs / (id + ".json");
    if (!overwrite && fs::exists(file, ec)) throw PatchError("exists", "song already exists: userdata/songs/" + id + ".json");
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw PatchError("io_error", "cannot write " + pathToUtf8(tmp));
        f << songToJson(song).dump(2) << '\n';
        if (!f) throw PatchError("io_error", "write failed: " + pathToUtf8(tmp));
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
        if (ec) throw PatchError("io_error", "cannot move " + pathToUtf8(tmp) + ": " + ec.message());
    }
    return paths.relativeToRoot(file);
}

void remove(const AppPaths& paths, const std::string& path) {
    if (isLogicalPath(path, "presets/songs/")) throw PatchError("read_only", "factory songs cannot be deleted");
    if (!isLogicalPath(path, "userdata/songs/")) throw PatchError("bad_path", "not a user song: " + path);
    auto resolved = paths.resolveAllowed(path);
    if (!resolved || resolved->extension() != ".json") throw PatchError("bad_path", "not a user song: " + path);
    std::error_code ec;
    if (!fs::exists(*resolved, ec)) throw PatchError("not_found", "no song " + path);
    if (!fs::remove(*resolved, ec) || ec) throw PatchError("io_error", "cannot delete " + path);
}

} // namespace songs

} // namespace ks
