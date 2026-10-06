#pragma once
// Patch data model (plain data, control thread). JSON mapping in preset/PatchJson.h, schema in docs/PRESETS.md.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ks {

using NodeId = uint32_t;
inline constexpr NodeId kMasterNode = 0; // reserved: the master chain

struct Zone {
    int keyLo = 0, keyHi = 127;
    int velLo = 1, velHi = 127;
    int transpose = 0; // semitones, applied after the key-range filter
    int channel = 0;   // 0 = omni, 1..16
    float volumeDb = 0.0f;
    float pan = 0.0f;  // -1..1
    bool mute = false, solo = false;
    bool sustain = true; // respond to CC64
};

struct ModuleSlot {
    NodeId node = 0;
    std::string type;
    std::map<std::string, float> params; // plain units; normalized against the module spec by PatchModel
    nlohmann::json state = nlohmann::json::object();
    bool bypass = false; // effects only
};

struct Layer {
    NodeId node = 0;
    std::string name;
    Zone zone;
    ModuleSlot instrument;
    std::vector<ModuleSlot> fx;
};

struct MasterSection {
    float volumeDb = 0.0f;
    std::vector<ModuleSlot> fx;
};

// Rhythm section (ARCHITECTURE §5.3 RhythmNode): the drum-sequencer kit + the pattern loaded with the patch.
struct RhythmSection {
    RhythmSection() { drums.type = "drums"; }
    ModuleSlot drums;    // type is always "drums"; node id assigned by PatchModel
    std::string pattern; // pattern path ("" = none)
};

struct PatchMeta {
    std::string name = "Init";
    std::string category;
    std::vector<std::string> tags;
    std::string description;
    std::string author;
};

struct Patch {
    int format = 1;
    PatchMeta meta;
    double tempo = 120.0;
    std::vector<Layer> layers;
    MasterSection master;
    RhythmSection rhythm;
    // Parse info (patchFromJson): whether the JSON had these keys. A preset without them keeps the current
    // rhythm section / tempo when loaded (PRESETS.md).
    bool hasRhythm = false;
    bool hasTempo = false;
};

} // namespace ks
