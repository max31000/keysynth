#include "control/Session.h"

#include "core/GraphBuilder.h"
#include "core/RtCheck.h"
#include "dsp/Math.h"
#include "preset/PatchJson.h"

#include <cmath>

namespace ks {

namespace {
float toDb(float peak) {
    const float db = dsp::gainToDb(peak);
    return std::round(std::fmax(db, -120.0f) * 10.0f) / 10.0f;
}
} // namespace

Session::Session(Engine& engine, const ModuleRegistry& registry, AppPaths paths)
    : engine_(engine), registry_(registry), paths_(paths), model_(registry), presets_(paths) {}

void Session::logWarnings(const std::vector<std::string>& w) {
    if (!log) return;
    for (const auto& s : w) log("warn", s);
}

void Session::rebuild(bool reuse) {
    BuildResult r = GraphBuilder::build(model_.patch(), registry_, reuse ? engine_.latestGraph() : nullptr,
                                        engine_.sampleRate(), engine_.maxBlock());
    logWarnings(r.warnings);
    engine_.transport().setTempo(model_.patch().tempo);
    engine_.publish(std::move(r.graph));
}

std::vector<std::string> Session::setPatch(Patch p, const std::string& presetPath, bool reuse) {
    auto w = model_.setPatch(std::move(p));
    presetPath_ = presetPath;
    logWarnings(w);
    rebuild(reuse);
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
            {"pattern", nullptr}, // drum sequencer: Phase 3
            {"drums", nullptr}};
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
            {"transport", {{"ppq", t.ppq.load()}, {"playing", engine_.transport().playing()}}},
            {"rtViolations", rt::violationCount()}};
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
