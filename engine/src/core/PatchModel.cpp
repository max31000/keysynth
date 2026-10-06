#include "core/PatchModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace ks {

namespace {
std::string fmtDb(float db) {
    char b[32];
    std::snprintf(b, sizeof b, "%+.1f", static_cast<double>(db));
    return b;
}
} // namespace

PatchModel::PatchModel(const ModuleRegistry& registry) : registry_(registry) { setPatch(makeDefaultPatch()); dirty_ = false; }

Patch PatchModel::makeDefaultPatch() {
    Patch p;
    p.meta.name = "Init";
    p.meta.category = "Synth Lead";
    Layer l;
    l.name = "Layer 1";
    l.instrument.type = "basic";
    p.layers.push_back(l);
    return p;
}

Zone PatchModel::sanitizeZone(Zone z) {
    auto c7 = [](int v) { return std::clamp(v, 0, 127); };
    z.keyLo = c7(z.keyLo);
    z.keyHi = c7(z.keyHi);
    z.velLo = std::clamp(z.velLo, 1, 127);
    z.velHi = std::clamp(z.velHi, 1, 127);
    z.transpose = std::clamp(z.transpose, -48, 48);
    z.channel = std::clamp(z.channel, 0, 16);
    if (!std::isfinite(z.volumeDb)) z.volumeDb = 0.0f;
    z.volumeDb = std::clamp(z.volumeDb, kMinVolumeDb, kMaxVolumeDb);
    if (!std::isfinite(z.pan)) z.pan = 0.0f;
    z.pan = std::clamp(z.pan, -1.0f, 1.0f);
    return z;
}

NodeId PatchModel::allocId() {
    if (nextId_ == 0 || nextId_ >= 0x7FFFFFFFu) nextId_ = 1; // ids stay < 2^31 (JSON-safe); 0 = master
    while (findSlot(nextId_) || findLayer(nextId_)) ++nextId_;
    return nextId_++;
}

void PatchModel::normalizeSlot(ModuleSlot& s, ModuleKind expected, std::vector<std::string>* warnings,
                               const std::string& where) {
    const ModuleInfo* info = registry_.find(s.type);
    if (!s.state.is_object() && !s.state.is_null()) s.state = nlohmann::json::object();
    if (s.state.is_null()) s.state = nlohmann::json::object();
    if (info == nullptr) {
        if (warnings) warnings->push_back(where + ": unknown module type '" + s.type + "' (kept, not loaded)");
        return;
    }
    if (info->kind != expected && warnings)
        warnings->push_back(where + ": module '" + s.type + "' has the wrong kind (kept, not loaded)");
    std::map<std::string, float> out;
    for (const auto& spec : info->params) {
        auto it = s.params.find(spec.id);
        out[spec.id] = spec.sanitize(it == s.params.end() ? spec.def : it->second);
    }
    if (warnings)
        for (const auto& [id, v] : s.params)
            if (!out.count(id)) warnings->push_back(where + ": unknown param '" + s.type + "." + id + "' ignored");
    s.params = std::move(out);
}

ModuleSlot PatchModel::makeSlot(const std::string& type, ModuleKind expected) {
    const ModuleInfo* info = registry_.find(type);
    if (info == nullptr) throw PatchError("unknown_module", "unknown module type '" + type + "'");
    if (info->kind != expected)
        throw PatchError("bad_request", "module '" + type + "' is not an " +
                                            (expected == ModuleKind::Instrument ? "instrument" : "effect"));
    ModuleSlot s;
    s.node = allocId();
    s.type = type;
    normalizeSlot(s, expected, nullptr, {});
    return s;
}

std::vector<std::string> PatchModel::setPatch(Patch p) {
    std::vector<std::string> warnings;
    if (p.layers.size() > static_cast<size_t>(kMaxLayers)) {
        warnings.push_back("too many layers; extra layers dropped");
        p.layers.resize(kMaxLayers);
    }
    // Node ids: keep unique non-zero ids, reassign the rest.
    std::set<NodeId> used;
    NodeId maxId = 0;
    auto claim = [&](NodeId& id) {
        if (id != 0 && id < 0x7FFFFFFFu && !used.count(id)) {
            used.insert(id);
            maxId = std::max(maxId, id);
        } else {
            id = 0;
        }
    };
    for (auto& l : p.layers) {
        claim(l.node);
        claim(l.instrument.node);
        for (auto& f : l.fx) claim(f.node);
    }
    for (auto& f : p.master.fx) claim(f.node);
    claim(p.rhythm.drums.node);
    nextId_ = maxId + 1;
    auto assign = [&](NodeId& id) {
        if (id == 0) id = allocId();
    };
    int li = 0;
    for (auto& l : p.layers) {
        ++li;
        const std::string where = "layer " + std::to_string(li);
        assign(l.node);
        assign(l.instrument.node);
        if (std::isfinite(l.zone.volumeDb) && (l.zone.volumeDb > kMaxVolumeDb || l.zone.volumeDb < kMinVolumeDb))
            warnings.push_back(where + ": volume_db " + fmtDb(l.zone.volumeDb) + " dB clamped");
        l.zone = sanitizeZone(l.zone);
        if (l.name.empty()) l.name = "Layer " + std::to_string(li);
        normalizeSlot(l.instrument, ModuleKind::Instrument, &warnings, where + " instrument");
        if (l.fx.size() > static_cast<size_t>(kMaxFxPerChain)) l.fx.resize(kMaxFxPerChain);
        for (auto& f : l.fx) {
            assign(f.node);
            normalizeSlot(f, ModuleKind::Effect, &warnings, where + " fx");
        }
    }
    if (p.master.fx.size() > static_cast<size_t>(kMaxFxPerChain)) p.master.fx.resize(kMaxFxPerChain);
    for (auto& f : p.master.fx) {
        assign(f.node);
        normalizeSlot(f, ModuleKind::Effect, &warnings, "master fx");
    }
    assign(p.rhythm.drums.node);
    p.rhythm.drums.type = "drums";
    p.rhythm.drums.bypass = false;
    normalizeSlot(p.rhythm.drums, ModuleKind::Instrument, &warnings, "rhythm kit");
    if (!std::isfinite(p.master.volumeDb)) p.master.volumeDb = 0.0f;
    if (p.master.volumeDb > kMaxVolumeDb || p.master.volumeDb < kMinVolumeDb)
        warnings.push_back("master volume_db " + fmtDb(p.master.volumeDb) + " dB clamped to [" +
                           std::to_string(static_cast<int>(kMinVolumeDb)) + ", +" +
                           std::to_string(static_cast<int>(kMaxVolumeDb)) + "] dB");
    p.master.volumeDb = std::clamp(p.master.volumeDb, kMinVolumeDb, kMaxVolumeDb);
    if (!std::isfinite(p.tempo) || p.tempo <= 0) p.tempo = 120.0;
    p.tempo = std::clamp(p.tempo, 20.0, 400.0);
    patch_ = std::move(p);
    dirty_ = false;
    return warnings;
}

Layer* PatchModel::layerMut(NodeId layer) {
    for (auto& l : patch_.layers)
        if (l.node == layer) return &l;
    return nullptr;
}

const Layer* PatchModel::findLayer(NodeId layer) const { return const_cast<PatchModel*>(this)->layerMut(layer); }

ModuleSlot* PatchModel::slotMut(NodeId node) {
    if (node == 0) return nullptr;
    for (auto& l : patch_.layers) {
        if (l.instrument.node == node) return &l.instrument;
        for (auto& f : l.fx)
            if (f.node == node) return &f;
    }
    for (auto& f : patch_.master.fx)
        if (f.node == node) return &f;
    if (patch_.rhythm.drums.node == node) return &patch_.rhythm.drums;
    return nullptr;
}

const ModuleSlot* PatchModel::findSlot(NodeId node) const { return const_cast<PatchModel*>(this)->slotMut(node); }

std::optional<NodeId> PatchModel::chainOf(NodeId fx) const {
    for (const auto& l : patch_.layers)
        for (const auto& f : l.fx)
            if (f.node == fx) return l.node;
    for (const auto& f : patch_.master.fx)
        if (f.node == fx) return kMasterNode;
    return std::nullopt;
}

std::vector<ModuleSlot>* PatchModel::chainMut(NodeId layerOrMaster) {
    if (layerOrMaster == kMasterNode) return &patch_.master.fx;
    if (auto* l = layerMut(layerOrMaster)) return &l->fx;
    return nullptr;
}

NodeId PatchModel::addLayer(std::optional<int> index, const std::string& instrumentType) {
    if (patch_.layers.size() >= static_cast<size_t>(kMaxLayers))
        throw PatchError("limit", "maximum number of layers reached");
    Layer l;
    l.instrument = makeSlot(instrumentType, ModuleKind::Instrument);
    l.node = allocId();
    l.name = "Layer " + std::to_string(patch_.layers.size() + 1);
    const int n = static_cast<int>(patch_.layers.size());
    const int at = index ? std::clamp(*index, 0, n) : n;
    patch_.layers.insert(patch_.layers.begin() + at, std::move(l));
    dirty_ = true;
    return patch_.layers[static_cast<size_t>(at)].node;
}

void PatchModel::removeLayer(NodeId layer) {
    auto it = std::find_if(patch_.layers.begin(), patch_.layers.end(), [&](const Layer& l) { return l.node == layer; });
    if (it == patch_.layers.end()) throw PatchError("not_found", "no layer " + std::to_string(layer));
    patch_.layers.erase(it);
    dirty_ = true;
}

void PatchModel::setInstrument(NodeId layer, const std::string& type) {
    Layer* l = layerMut(layer);
    if (!l) throw PatchError("not_found", "no layer " + std::to_string(layer));
    ModuleSlot s = makeSlot(type, ModuleKind::Instrument);
    l->instrument = std::move(s);
    dirty_ = true;
}

NodeId PatchModel::addFx(NodeId layer, const std::string& type, std::optional<int> index) {
    auto* chain = chainMut(layer);
    if (!chain) throw PatchError("not_found", "no layer " + std::to_string(layer));
    if (chain->size() >= static_cast<size_t>(kMaxFxPerChain)) throw PatchError("limit", "fx chain is full");
    ModuleSlot s = makeSlot(type, ModuleKind::Effect);
    const int n = static_cast<int>(chain->size());
    const int at = index ? std::clamp(*index, 0, n) : n;
    const NodeId id = s.node;
    chain->insert(chain->begin() + at, std::move(s));
    dirty_ = true;
    return id;
}

void PatchModel::removeFx(NodeId fx) {
    auto owner = chainOf(fx);
    if (!owner) throw PatchError("not_found", "no fx " + std::to_string(fx));
    auto* chain = chainMut(*owner);
    chain->erase(std::find_if(chain->begin(), chain->end(), [&](const ModuleSlot& s) { return s.node == fx; }));
    dirty_ = true;
}

void PatchModel::moveFx(NodeId fx, int to) {
    auto owner = chainOf(fx);
    if (!owner) throw PatchError("not_found", "no fx " + std::to_string(fx));
    auto* chain = chainMut(*owner);
    auto it = std::find_if(chain->begin(), chain->end(), [&](const ModuleSlot& s) { return s.node == fx; });
    ModuleSlot s = std::move(*it);
    chain->erase(it);
    const int n = static_cast<int>(chain->size());
    chain->insert(chain->begin() + std::clamp(to, 0, n), std::move(s));
    dirty_ = true;
}

void PatchModel::setModuleState(NodeId node, const nlohmann::json& state) {
    ModuleSlot* s = slotMut(node);
    if (!s) throw PatchError("not_found", "no node " + std::to_string(node));
    s->state = state.is_object() ? state : nlohmann::json::object();
    dirty_ = true;
}

void PatchModel::setZone(NodeId layer, const Zone& zone) {
    Layer* l = layerMut(layer);
    if (!l) throw PatchError("not_found", "no layer " + std::to_string(layer));
    l->zone = sanitizeZone(zone);
    dirty_ = true;
}

void PatchModel::setFxBypass(NodeId fx, bool bypass) {
    if (!chainOf(fx)) throw PatchError("not_found", "no fx " + std::to_string(fx));
    slotMut(fx)->bypass = bypass;
    dirty_ = true;
}

float PatchModel::setParam(NodeId node, const std::string& param, float value) {
    ModuleSlot* s = slotMut(node);
    if (!s) throw PatchError("not_found", "no node " + std::to_string(node));
    const ModuleInfo* info = registry_.find(s->type);
    if (!info) throw PatchError("unknown_module", "module '" + s->type + "' is not loaded");
    for (const auto& spec : info->params) {
        if (spec.id == param) {
            if (spec.isReadOnly()) throw PatchError("read_only", "param '" + param + "' is read-only");
            const float v = spec.sanitize(value);
            s->params[param] = v;
            dirty_ = true;
            return v;
        }
    }
    throw PatchError("unknown_param", "module '" + s->type + "' has no param '" + param + "'");
}

void PatchModel::setMasterVolume(float db) {
    patch_.master.volumeDb = std::isfinite(db) ? std::clamp(db, kMinVolumeDb, kMaxVolumeDb) : 0.0f;
    dirty_ = true;
}

void PatchModel::setTempo(double bpm) {
    if (std::isfinite(bpm)) patch_.tempo = std::clamp(bpm, 20.0, 400.0);
    dirty_ = true;
}

void PatchModel::setRhythmPattern(const std::string& path) {
    if (patch_.rhythm.pattern != path) dirty_ = true;
    patch_.rhythm.pattern = path;
}

void PatchModel::setMeta(const PatchMeta& meta) {
    patch_.meta = meta;
    dirty_ = true;
}

} // namespace ks
