#include "control/Session.h"

#include "core/GraphBuilder.h"
#include "core/RtCheck.h"
#include "dsp/Math.h"
#include "preset/PatchJson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ks {

namespace {
float toDb(float peak) {
    const float db = dsp::gainToDb(peak);
    return std::round(std::fmax(db, -120.0f) * 10.0f) / 10.0f;
}
} // namespace

Session::Session(Engine& engine, const ModuleRegistry& registry, AppPaths paths)
    : engine_(engine), registry_(registry), paths_(paths), model_(registry), presets_(paths) {
    loadSongSettings();
    pattern_ = emptyPattern();
    publishPattern();
    // Default groove (absent in stripped-down test roots: stays empty).
    const std::string def = "presets/patterns/basic-rock.json";
    std::error_code ec;
    if (std::filesystem::exists(paths_.root / "presets" / "patterns" / "basic-rock.json", ec)) {
        try {
            loadPattern(def, false);
            model_.setDirty(false);
        } catch (const std::exception&) {
        }
    }
}

Pattern Session::emptyPattern() {
    Pattern p;
    p.name = "Empty";
    const std::pair<const char*, int> tracks[] = {{"Kick", 36},   {"Snare", 38},  {"Clap", 39},  {"Closed Hat", 42},
                                                  {"Open Hat", 46}, {"Low Tom", 45}, {"High Tom", 50}, {"Crash", 49}};
    for (const auto& [n, note] : tracks) {
        PatternTrack t;
        t.name = n;
        t.note = note;
        p.tracks.push_back(t);
    }
    p.normalize();
    return p;
}

void Session::publishPattern() {
    engine_.sequencer().setPattern(toRt(pattern_));
    engine_.transport().setTimeSignature(pattern_.numerator, pattern_.denominator);
}

Pattern Session::readPatternFile(const std::string& path, std::vector<std::string>* warnings) const {
    auto resolved = paths_.resolveAllowed(path);
    if (!resolved) throw PatchError("bad_path", "path is outside the allowed roots: " + path);
    if (resolved->extension() != ".json") throw PatchError("bad_path", "patterns must be .json files");
    std::ifstream f(*resolved, std::ios::binary);
    if (!f) throw PatchError("not_found", "cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw PatchError("parse_error", path + ": invalid pattern JSON");
    return patternFromJson(j, warnings);
}

std::vector<Session::PatternInfo> Session::listPatterns() const {
    namespace fs = std::filesystem;
    std::vector<PatternInfo> out;
    const std::pair<fs::path, bool> dirs[] = {{paths_.factoryPatterns, true}, {paths_.userPatterns, false}};
    for (const auto& [dir, factory] : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            try {
                const std::string rel = paths_.relativeToRoot(file);
                const Pattern p = readPatternFile(rel);
                out.push_back({rel, p.name, p.numerator, p.denominator, p.bars, p.tempo, factory});
            } catch (const std::exception&) {
                // unreadable files are skipped
            }
        }
    }
    return out;
}

std::vector<std::string> Session::loadPattern(const std::string& path, bool applyMeta) {
    std::vector<std::string> w;
    Pattern p = path.empty() ? emptyPattern() : readPatternFile(path, &w);
    std::string rel = path;
    if (!path.empty())
        if (auto r = paths_.resolveAllowed(path)) rel = paths_.relativeToRoot(*r);
    pattern_ = std::move(p);
    patternPath_ = rel;
    patternEdited_ = false;
    publishPattern();
    engine_.rhythm().swing.store(pattern_.swing, std::memory_order_relaxed);
    if (applyMeta) {
        if (pattern_.tempo > 0) setTempo(pattern_.tempo);
        if (pattern_.kit >= 0 && model_.patch().rhythm.drums.node != 0)
            setParam(model_.patch().rhythm.drums.node, "kit", static_cast<float>(pattern_.kit));
    }
    model_.setRhythmPattern(rel);
    logWarnings(w);
    return w;
}

void Session::setPattern(Pattern p) {
    auto w = p.normalize();
    logWarnings(w);
    pattern_ = std::move(p);
    patternEdited_ = true;
    publishPattern();
    engine_.rhythm().swing.store(pattern_.swing, std::memory_order_relaxed);
}

std::string Session::savePattern(const std::string& name, bool overwrite) {
    namespace fs = std::filesystem;
    if (name.empty()) throw PatchError("bad_request", "pattern name is empty");
    std::error_code ec;
    const fs::path dir = paths_.userPatterns;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (PresetStore::slugify(name) + ".json");
    if (!overwrite && fs::exists(file, ec)) throw PatchError("exists", "pattern already exists: " + pathToUtf8(file));
    Pattern p = pattern_;
    p.name = name;
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        if (!f) throw PatchError("io_error", "cannot write " + pathToUtf8(file));
        f << patternToJson(p).dump(2) << "\n";
        if (!f) throw PatchError("io_error", "write failed: " + pathToUtf8(file));
    }
    pattern_.name = name;
    patternPath_ = paths_.relativeToRoot(file);
    patternEdited_ = false;
    model_.setRhythmPattern(patternPath_);
    return patternPath_;
}

void Session::setTimeSignature(int num, int den) {
    pattern_.setTimeSignature(num, den);
    patternEdited_ = true;
    publishPattern();
}

void Session::setSwing(float swing) {
    if (!std::isfinite(swing)) return;
    pattern_.swing = std::clamp(swing, 0.0f, 1.0f);
    engine_.rhythm().swing.store(pattern_.swing, std::memory_order_relaxed);
}

nlohmann::json Session::patternJson() const {
    return {{"path", patternPath_}, {"edited", patternEdited_}, {"pattern", patternToJson(pattern_)}};
}

void Session::logWarnings(const std::vector<std::string>& w) {
    if (!log) return;
    for (const auto& s : w) log("warn", s, false);
}

void Session::rebuild(bool reuse) {
    BuildResult r = GraphBuilder::build(model_.patch(), registry_, reuse ? engine_.latestGraph() : nullptr,
                                        engine_.sampleRate(), engine_.maxBlock());
    logWarnings(r.warnings);
    for (const auto& a : r.adopted) model_.adoptModuleState(a.node, a.params, a.state);
    engine_.transport().setTempo(model_.patch().tempo);
    engine_.publish(std::move(r.graph));
}

bool Session::refreshModules(const std::vector<std::string>& typeIds) {
    auto affected = [&](const std::string& t) { return std::find(typeIds.begin(), typeIds.end(), t) != typeIds.end(); };
    Patch p = model_.patch();
    bool any = false;
    RackGraph* g = engine_.latestGraph();
    auto visit = [&](ModuleSlot& s) {
        if (!affected(s.type)) return;
        any = true;
        if (!g) return;
        if (Module* m = g->findModule(s.node)) {
            nlohmann::json st = m->saveState();
            if (st.is_object() && !st.empty()) s.state = std::move(st);
        }
    };
    for (Layer& l : p.layers) {
        visit(l.instrument);
        for (ModuleSlot& f : l.fx) visit(f);
    }
    for (ModuleSlot& f : p.master.fx) visit(f);
    if (!any) return false;
    const bool dirty = model_.dirty();
    model_.setPatch(std::move(p)); // node ids are kept (all valid); params re-normalized
    model_.setDirty(dirty);
    rebuild(true);
    return true;
}

std::vector<std::string> Session::setPatch(Patch p, const std::string& presetPath, bool reuse) {
    // A patch without a rhythm section / tempo keeps the current ones (PRESETS.md).
    if (!p.hasRhythm) p.rhythm = model_.patch().rhythm;
    if (!p.hasTempo) p.tempo = model_.patch().tempo;
    if (p.hasRhythm && !p.hasRhythmPattern) p.rhythm.pattern = patternPath_; // kit only: keep the groove
    const std::string pattern = p.rhythm.pattern;
    const bool loadIt = p.hasRhythm && pattern != patternPath_;
    auto w = model_.setPatch(std::move(p));
    presetPath_ = presetPath;
    logWarnings(w);
    rebuild(reuse);
    if (loadIt) {
        try {
            auto pw = loadPattern(pattern, false);
            w.insert(w.end(), pw.begin(), pw.end());
        } catch (const PatchError& e) {
            w.push_back(std::string("rhythm.pattern: ") + e.what());
            logWarnings({w.back()});
            model_.setRhythmPattern(patternPath_); // the patch names what actually plays
        }
    }
    return w;
}

std::vector<std::string> Session::loadPreset(const std::string& path) {
    std::vector<std::string> w;
    Patch p = presets_.load(path, &w);
    auto resolved = paths_.resolveAllowed(path);
    auto w2 = setPatch(std::move(p), resolved ? paths_.relativeToRoot(*resolved) : path, false);
    w.insert(w.end(), w2.begin(), w2.end());
    return w;
}

std::string Session::savePreset(const std::string& name, const std::string& category, bool overwrite) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(profileMutex_);
        path = presets_.save(model_.patch(), name, category, overwrite);
    }
    if (onUserPresetsChanged) onUserPresetsChanged();
    PatchMeta meta = model_.patch().meta;
    meta.name = name;
    if (!category.empty()) meta.category = category;
    model_.setMeta(meta);
    model_.setDirty(false);
    presetPath_ = path;
    return path;
}

float Session::setParam(NodeId node, const std::string& param, float value) {
    if (node == kMasterNode && param == "volume_db") {
        setMasterVolume(value);
        return model_.patch().master.volumeDb;
    }
    const float v = model_.setParam(node, param, value);
    if (RackGraph* g = engine_.latestGraph())
        if (Module* m = g->findModule(node)) m->params().set(param, v);
    return v;
}

void Session::setZone(NodeId layer, const Zone& zone) {
    model_.setZone(layer, zone);
    if (RackGraph* g = engine_.latestGraph())
        if (LayerNode* l = g->findLayer(layer)) l->setZone(model_.findLayer(layer)->zone);
}

void Session::setFxBypass(NodeId node, bool bypass) {
    model_.setFxBypass(node, bypass);
    if (RackGraph* g = engine_.latestGraph())
        if (ModuleNode* n = g->findModuleNode(node)) n->bypass.store(bypass);
}

void Session::setMasterVolume(float db) {
    model_.setMasterVolume(db);
    if (RackGraph* g = engine_.latestGraph()) g->masterVolumeDb.store(model_.patch().master.volumeDb);
}

void Session::setTempo(double bpm) {
    model_.setTempo(bpm);
    engine_.transport().setTempo(model_.patch().tempo);
}

bool Session::setProfile(const std::string& accountId) {
    std::lock_guard<std::mutex> lk(profileMutex_);
    if (!paths_.setProfile(accountId)) return false;
    presets_ = PresetStore(paths_);
    // Wire paths `userdata/...` now mean the new profile: forget references into the old one.
    if (presetPath_.rfind("userdata/", 0) == 0) presetPath_.clear();
    if (patternPath_.rfind("userdata/", 0) == 0) {
        patternPath_.clear();
        patternEdited_ = true;
    }
    if (songPath_.rfind("userdata/", 0) == 0) {
        activeSong_.reset();
        songPath_.clear();
        songSection_ = -1;
        applyControlKeys();
    }
    return true;
}

// --- MIDI tools ------------------------------------------------------------------------------------------------

std::vector<int> Session::learnChord(NodeId node, std::vector<std::pair<std::string, float>>* changed) {
    const ModuleSlot* slot = model_.findSlot(node);
    if (!slot) throw PatchError("not_found", "no node " + std::to_string(node));
    if (slot->type != "chord") throw PatchError("bad_request", "node " + std::to_string(node) + " is not a chord tool");
    RackGraph* g = engine_.latestGraph();
    Module* m = g ? g->findModule(node) : nullptr;
    if (!m) throw PatchError("not_found", "chord tool " + std::to_string(node) + " is not running");
    const ParamSet& ps = m->params();
    const int ci = ps.indexOf("held_count");
    const int count = ci >= 0 ? static_cast<int>(ps.get(ci)) : 0;
    std::vector<int> notes;
    for (int k = 1; k <= 8 && k <= count; ++k) {
        const int i = ps.indexOf("held_" + std::to_string(k));
        const int v = i >= 0 ? static_cast<int>(ps.get(i)) : -1;
        if (v >= 0) notes.push_back(v);
    }
    if (notes.size() < 2) throw PatchError("no_notes", "hold at least two keys on this layer, then learn");
    std::sort(notes.begin(), notes.end());
    std::vector<int> intervals;
    for (int n : notes) intervals.push_back(std::min(48, n - notes.front()));
    auto set = [&](const std::string& id, float v) {
        const float s = setParam(node, id, v);
        if (changed) changed->emplace_back(id, s);
    };
    set("mem_count", static_cast<float>(intervals.size()));
    for (int k = 1; k <= 8; ++k)
        set("mem_" + std::to_string(k), k <= static_cast<int>(intervals.size()) ? static_cast<float>(intervals[static_cast<size_t>(k - 1)]) : 0.0f);
    const ParamSpec* typeSpec = nullptr;
    if (const ModuleInfo* info = registry_.find("chord"))
        for (const auto& sp : info->params)
            if (sp.id == "type") typeSpec = &sp;
    if (typeSpec && !typeSpec->choices.empty()) set("type", static_cast<float>(typeSpec->choices.size() - 1)); // Memory
    return intervals;
}

// --- songs -----------------------------------------------------------------------------------------------------

void Session::loadSongSettings() {
    std::ifstream f(paths_.settingsFile, std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return;
    const auto s = j.find("songs");
    if (s == j.end() || !s->is_object()) return;
    if (auto k = s->find("control_keys"); k != s->end()) {
        try {
            globalKeys_ = controlKeysFromJson(*k, globalKeys_);
        } catch (const PatchError&) {
        }
    }
}

void Session::setGlobalControlKeys(const ControlKeys& keys) {
    globalKeys_ = keys;
    applyControlKeys();
    // Read-modify-write: settings.json also holds the audio settings (AudioHost).
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream f(paths_.settingsFile, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            j = nlohmann::json::parse(ss.str(), nullptr, false);
            if (!j.is_object()) j = nlohmann::json::object();
        }
    }
    if (!j["songs"].is_object()) j["songs"] = nlohmann::json::object();
    j["songs"]["control_keys"] = controlKeysToJson(globalKeys_);
    std::error_code ec;
    std::filesystem::create_directories(paths_.settingsFile.parent_path(), ec);
    std::ofstream f(paths_.settingsFile, std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(2) << '\n';
    if (!f) throw PatchError("io_error", "cannot write " + pathToUtf8(paths_.settingsFile));
}

ControlKeys Session::effectiveControlKeys() const {
    if (activeSong_ && activeSong_->controlKeys) return *activeSong_->controlKeys;
    return globalKeys_;
}

void Session::applyControlKeys() {
    const ControlKeys k = effectiveControlKeys();
    const bool on = activeSong_.has_value() && k.enabled;
    seenPresses_[0] = engine_.controlKeyPresses(0);
    seenPresses_[1] = engine_.controlKeyPresses(1);
    engine_.setControlKeys(on ? k.prev : -1, on ? k.next : -1);
}

nlohmann::json Session::songStateJson() const {
    const ControlKeys k = effectiveControlKeys();
    nlohmann::json ck = controlKeysToJson(k);
    ck["source"] = activeSong_ && activeSong_->controlKeys ? "song" : "global";
    return {{"type", "song_state"},
            {"path", activeSong_ ? nlohmann::json(songPath_) : nlohmann::json(nullptr)},
            {"song", activeSong_ ? songToJson(*activeSong_) : nlohmann::json(nullptr)},
            {"section", activeSong_ ? songSection_ : -1},
            {"controlKeys", ck},
            {"globalControlKeys", controlKeysToJson(globalKeys_)}};
}

std::string Session::saveSong(Song song, bool overwrite, bool* active) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(profileMutex_);
        path = songs::save(paths_, std::move(song), overwrite);
    }
    if (onUserPresetsChanged) onUserPresetsChanged();
    const bool isActive = activeSong_ && path == songPath_;
    if (isActive) {
        activeSong_ = songs::load(paths_, path);
        if (songSection_ >= static_cast<int>(activeSong_->sections.size()))
            songSection_ = static_cast<int>(activeSong_->sections.size()) - 1;
        applyControlKeys();
    }
    if (active) *active = isActive;
    return path;
}

void Session::deleteSong(const std::string& path, bool* active) {
    {
        std::lock_guard<std::mutex> lk(profileMutex_);
        songs::remove(paths_, path);
    }
    if (onUserPresetsChanged) onUserPresetsChanged();
    const bool isActive = activeSong_ && path == songPath_;
    if (isActive) closeSong();
    if (active) *active = isActive;
}

std::vector<std::string> Session::selectSong(const std::string& path, int section) {
    Song s = songs::load(paths_, path);
    if (s.sections.empty()) throw PatchError("bad_request", "the song has no sections");
    if (section < 0 || section >= static_cast<int>(s.sections.size()))
        throw PatchError("bad_request", "section index out of range");
    auto resolved = paths_.resolveAllowed(path);
    activeSong_ = std::move(s);
    songPath_ = resolved ? paths_.relativeToRoot(*resolved) : path;
    songSection_ = -1;
    applyControlKeys();
    return selectSection(section);
}

std::vector<std::string> Session::selectSection(int index) {
    if (!activeSong_) throw PatchError("no_song", "no song selected");
    // Re-read the file: sync may have pulled a newer version meanwhile (a vanished/broken file keeps the copy).
    try {
        Song fresh = songs::load(paths_, songPath_);
        if (!fresh.sections.empty()) activeSong_ = std::move(fresh);
    } catch (const PatchError&) {
    }
    const Song& song = *activeSong_;
    if (index < 0 || index >= static_cast<int>(song.sections.size()))
        throw PatchError("bad_request", "section index out of range");
    const SongSection& sec = song.sections[static_cast<size_t>(index)];
    songSection_ = index;
    std::vector<std::string> w;
    auto fail = [&](const std::string& what, const std::exception& e) {
        w.push_back(sec.name + ": " + what + ": " + e.what());
        if (log) log("error", w.back(), true);
    };
    if (sec.preset) {
        const std::string p = presetRefToPath(*sec.preset);
        try {
            if (p.empty()) throw PatchError("bad_path", "invalid preset reference '" + *sec.preset + "'");
            // Same preset, unedited: keep it (no reload click, tails survive).
            if (p != presetPath_ || model_.dirty()) loadPreset(p);
        } catch (const std::exception& e) {
            fail("preset", e);
        }
    }
    const bool reloadPattern = sec.pattern && (*sec.pattern != patternPath_ || patternEdited_);
    if (song.timeSig && (song.timeSig->first != pattern_.numerator || song.timeSig->second != pattern_.denominator))
        setTimeSignature(song.timeSig->first, song.timeSig->second);
    if (sec.pattern) {
        try {
            if (!isPatternRef(*sec.pattern)) throw PatchError("bad_path", "invalid pattern path '" + *sec.pattern + "'");
            if (reloadPattern) loadPattern(*sec.pattern, true);
        } catch (const std::exception& e) {
            fail("pattern", e);
        }
    }
    if (const auto t = sec.tempo ? sec.tempo : song.tempo) setTempo(*t);
    if (sec.drums) {
        if (*sec.drums) {
            engine_.rhythm().drumsEnabled.store(true);
            if (!engine_.transport().playing()) engine_.transport().setPlaying(true);
        } else {
            engine_.transport().setPlaying(false);
        }
    }
    return w;
}

bool Session::stepSection(int delta, std::vector<std::string>* warnings) {
    if (!activeSong_) throw PatchError("no_song", "no song selected");
    const int next = songSection_ + delta;
    if (next < 0 || next >= static_cast<int>(activeSong_->sections.size())) return false;
    auto w = selectSection(next);
    if (warnings) *warnings = std::move(w);
    return true;
}

void Session::closeSong() {
    activeSong_.reset();
    songPath_.clear();
    songSection_ = -1;
    applyControlKeys();
}

bool Session::pollSongKeys() {
    const uint32_t p = engine_.controlKeyPresses(0), n = engine_.controlKeyPresses(1);
    const uint32_t dp = p - seenPresses_[0], dn = n - seenPresses_[1];
    seenPresses_[0] = p;
    seenPresses_[1] = n;
    if (!activeSong_ || (dp == 0 && dn == 0)) return false;
    // Several presses within one pump tick: apply only the resulting section (one preset load).
    const int last = static_cast<int>(activeSong_->sections.size()) - 1;
    const int target = std::clamp(songSection_ - static_cast<int>(std::min<uint32_t>(dp, 64)) +
                                      static_cast<int>(std::min<uint32_t>(dn, 64)),
                                  0, last);
    if (target == songSection_) return false;
    try {
        selectSection(target);
    } catch (const PatchError&) {
        return false;
    }
    return true;
}

void Session::notifyResourcesChanged() {
    if (RackGraph* g = engine_.latestGraph())
        for (ModuleNode* n : g->allModuleNodes())
            if (n->module) n->module->resourcesChanged();
}

std::set<std::string> Session::samplerEntriesInUse() const {
    std::set<std::string> out;
    const std::string prefix = "assets/samples/";
    auto visit = [&](const ModuleSlot& s) {
        if (s.type != "sampler" || !s.state.is_object()) return;
        auto it = s.state.find("sfz");
        if (it == s.state.end() || !it->is_string()) return;
        const std::string sfz = it->get<std::string>();
        if (sfz.rfind(prefix, 0) == 0) out.insert(sfz.substr(prefix.size()));
    };
    for (const Layer& l : model_.patch().layers) {
        visit(l.instrument);
        for (const ModuleSlot& f : l.fx) visit(f);
    }
    return out;
}

nlohmann::json Session::transportJson() const {
    auto& e = const_cast<Engine&>(engine_);
    return {{"playing", e.transport().playing()},
            {"tempo", e.transport().tempo()},
            {"metronome", e.metronome().enabled()},
            {"metronome_volume", e.metronome().volume()},
            {"pattern", patternPath_},
            {"pattern_edited", patternEdited_},
            {"drums", e.rhythm().drumsEnabled.load()},
            {"drums_volume", e.rhythm().drumsVolume.load()},
            {"swing", e.rhythm().swing.load()},
            {"time_sig", {e.transport().numerator(), e.transport().denominator()}},
            {"count_in", e.rhythm().countIn.load()}};
}

nlohmann::json Session::stateJson() const {
    AudioStatus st;
    if (audio_) st = audio_->status();
    return {{"type", "state"},
            {"patch", patchToJson(model_.patch())},
            {"presetPath", presetPath_},
            {"dirty", model_.dirty()},
            {"audio", toJson(st)},
            {"transport", transportJson()}};
}

nlohmann::json Session::devicesJson() {
    nlohmann::json types = nlohmann::json::array(), available = nlohmann::json::array();
    AudioStatus st;
    if (audio_) {
        const AudioDeviceList l = audio_->listDevices();
        types = l.types;
        for (const auto& t : l.available)
            available.push_back({{"type", t.type}, {"names", t.names}, {"sampleRates", t.sampleRates}, {"bufferSizes", t.bufferSizes}});
        st = audio_->status();
    }
    nlohmann::json midi = nlohmann::json::array();
    if (midi_) midi = midi_->inputs();
    return {{"type", "devices"}, {"types", types}, {"current", toJson(st)}, {"available", available}, {"midiInputs", midi}};
}

std::string Session::missingSamplesMessage(const std::string& presetName, const std::string& reason) {
    const std::string what = presetName.empty() ? std::string("Сэмплы") : "Сэмплы для «" + presetName + "»";
    return reason == "offline" ? what + " не скачаны: нет связи с сервером, повторю позже"
                               : what + " не скачаны — привяжите движок к аккаунту на mvv42.ru (кнопка «Подключить»)";
}

int Session::pollModuleErrors() {
    std::map<const Module*, std::string> now;
    int reported = 0;
    if (RackGraph* g = engine_.latestGraph()) {
        for (ModuleNode* n : g->allModuleNodes()) {
            if (!n->module) continue;
            if (const auto miss = n->module->missingResource()) {
                if (notifiedMissing_.insert(miss->reason + "|" + miss->id).second) {
                    std::printf("[samples] %s (node %u): %s not downloaded (%s): %s\n", n->type.c_str(), n->node,
                                miss->id.c_str(), miss->reason.c_str(), miss->detail.c_str());
                    std::fflush(stdout);
                    if (log) log("warn", missingSamplesMessage(model_.patch().meta.name, miss->reason), true);
                    ++reported;
                }
                continue;
            }
            std::string err = n->module->loadError();
            if (err.empty()) continue;
            const auto it = reportedErrors_.find(n->module.get());
            if (it == reportedErrors_.end() || it->second != err) {
                if (log) log("error", n->type + " (node " + std::to_string(n->node) + "): " + err, true);
                ++reported;
            }
            now.emplace(n->module.get(), std::move(err));
        }
    }
    reportedErrors_ = std::move(now); // modules that left the graph are forgotten
    return reported;
}

nlohmann::json Session::pollTelemetry() {
    Telemetry& t = engine_.telemetry();
    const float cpu = t.cpuLoad.exchange(0.0f);
    nlohmann::json layers = nlohmann::json::object();
    const int nl = t.layerCount.load();
    for (int i = 0; i < nl && i < Telemetry::kMaxLayers; ++i) {
        auto& m = t.layers[static_cast<size_t>(i)];
        layers[std::to_string(m.node.load())] = {toDb(m.l.exchange(0.0f)), toDb(m.r.exchange(0.0f))};
    }
    nlohmann::json readouts = nlohmann::json::object();
    if (RackGraph* g = engine_.latestGraph()) {
        for (ModuleNode* n : g->allModuleNodes()) {
            if (!n->module) continue;
            const ParamSet& ps = n->module->params();
            nlohmann::json r = nlohmann::json::object();
            for (int i = 0; i < ps.size(); ++i)
                if (ps.spec(i).isReadOnly()) r[ps.spec(i).id] = ps.get(i);
            if (!r.empty()) readouts[std::to_string(n->node)] = r;
        }
    }
    const uint64_t xr = audio_ ? audio_->xruns() : t.overloads.load();
    return {{"type", "telemetry"},
            {"cpu", std::round(std::fmin(static_cast<double>(cpu), 1.0) * 1000.0) / 1000.0},
            {"xruns", xr},
            {"voices", t.voices.load()},
            {"meters", {{"master", {toDb(t.masterL.exchange(0.0f)), toDb(t.masterR.exchange(0.0f))}}, {"layers", layers}}},
            {"readouts", readouts},
            {"transport", transportTelemetry()},
            {"rtViolations", rt::violationCount()}};
}

nlohmann::json Session::transportTelemetry() const {
    auto& e = const_cast<Engine&>(engine_);
    const int step = e.currentStep();
    const int spb = std::max(1, pattern_.stepsPerBar());
    return {{"ppq", e.telemetry().ppq.load()},
            {"playing", e.transport().playing()},
            {"step", step},
            {"bar", step >= 0 ? step / spb : -1}};
}

nlohmann::json Session::pollMidi() {
    nlohmann::json notes = nlohmann::json::array();
    TelemetryEvent e;
    int n = 0;
    while (n < 512 && engine_.telemetry().events.pop(e)) {
        ++n;
        if (e.type == TelemetryEvent::Type::NoteOn || e.type == TelemetryEvent::Type::NoteOff)
            notes.push_back({{"note", e.note}, {"on", e.type == TelemetryEvent::Type::NoteOn}, {"velocity", e.velocity}});
    }
    if (notes.empty()) return nullptr;
    return {{"type", "midi"}, {"notes", notes}};
}

} // namespace ks
