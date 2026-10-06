#include "control/Session.h"

#include "core/GraphBuilder.h"
#include "core/RtCheck.h"
#include "dsp/Math.h"
#include "preset/PatchJson.h"

#include <algorithm>
#include <cmath>
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
    const std::pair<fs::path, bool> dirs[] = {{paths_.root / "presets" / "patterns", true},
                                              {paths_.userdata / "patterns", false}};
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
    const fs::path dir = paths_.userdata / "patterns";
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
    for (const auto& s : w) log("warn", s);
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
    const std::string path = presets_.save(model_.patch(), name, category, overwrite);
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

int Session::pollModuleErrors() {
    std::map<const Module*, std::string> now;
    int reported = 0;
    if (RackGraph* g = engine_.latestGraph()) {
        for (ModuleNode* n : g->allModuleNodes()) {
            if (!n->module) continue;
            std::string err = n->module->loadError();
            if (err.empty()) continue;
            const auto it = reportedErrors_.find(n->module.get());
            if (it == reportedErrors_.end() || it->second != err) {
                if (log) log("error", n->type + " (node " + std::to_string(n->node) + "): " + err);
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
