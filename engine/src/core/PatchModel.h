#pragma once
// PatchModel: single source of truth for the patch (control thread only, ARCHITECTURE §5.3).
// Assigns stable node ids, normalizes params against module specs, and implements every patch edit.
// Errors are reported by throwing PatchError (control thread only; never on the audio thread).

#include "core/ModuleRegistry.h"
#include "core/Patch.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ks {

class PatchError : public std::runtime_error {
public:
    PatchError(std::string code, const std::string& message) : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

class PatchModel {
public:
    static constexpr int kMaxLayers = 16;
    static constexpr int kMaxFxPerChain = 16;
    // Layer / master `volume_db` range; out-of-range values are clamped (with a load warning in setPatch).
    static constexpr float kMinVolumeDb = -96.0f;
    static constexpr float kMaxVolumeDb = 12.0f;

    explicit PatchModel(const ModuleRegistry& registry);

    const Patch& patch() const noexcept { return patch_; }
    const ModuleRegistry& registry() const noexcept { return registry_; }

    // Replace the whole patch: assigns missing/duplicate node ids, fills missing params with defaults, drops
    // unknown params. Returns human-readable warnings (unknown modules/params...).
    std::vector<std::string> setPatch(Patch p);

    // Default patch: one `basic` layer.
    static Patch makeDefaultPatch();

    // --- structural edits (caller rebuilds the graph) ---
    NodeId addLayer(std::optional<int> index, const std::string& instrumentType = "basic");
    void removeLayer(NodeId layer);
    void setInstrument(NodeId layer, const std::string& type);
    NodeId addFx(NodeId layer, const std::string& type, std::optional<int> index); // layer 0 = master
    void removeFx(NodeId fx);
    void moveFx(NodeId fx, int to);
    void setModuleState(NodeId node, const nlohmann::json& state);
    // A module's loadState wrote params (fm .syx voice, Module::loadStateWroteParams): take its params and state
    // into the patch. Does not mark the patch dirty. False if the node is gone.
    bool adoptModuleState(NodeId node, const std::map<std::string, float>& params, const nlohmann::json& state);

    // --- param-like edits (no rebuild; caller updates the live graph) ---
    void setZone(NodeId layer, const Zone& zone); // sanitized
    void setFxBypass(NodeId fx, bool bypass);
    float setParam(NodeId node, const std::string& param, float value); // returns the sanitized value
    void setMasterVolume(float db);
    void setTempo(double bpm);
    void setMeta(const PatchMeta& meta);
    // Rhythm section: pattern path stored with the patch (kit params go through setParam).
    void setRhythmPattern(const std::string& path);

    // --- queries ---
    const Layer* findLayer(NodeId layer) const;
    const ModuleSlot* findSlot(NodeId node) const; // instrument, fx (any chain) or the rhythm kit
    // Chain owning an fx node: layer node id, or kMasterNode. nullopt if not an fx.
    std::optional<NodeId> chainOf(NodeId fx) const;

    bool dirty() const noexcept { return dirty_; }
    void setDirty(bool d) noexcept { dirty_ = d; }

    static Zone sanitizeZone(Zone z);

private:
    NodeId allocId();
    Layer* layerMut(NodeId layer);
    ModuleSlot* slotMut(NodeId node);
    std::vector<ModuleSlot>* chainMut(NodeId layerOrMaster);
    void normalizeSlot(ModuleSlot& s, ModuleKind expected, std::vector<std::string>* warnings, const std::string& where);
    ModuleSlot makeSlot(const std::string& type, ModuleKind expected);

    const ModuleRegistry& registry_;
    Patch patch_;
    NodeId nextId_ = 1;
    bool dirty_ = false;
};

} // namespace ks
