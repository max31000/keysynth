#pragma once
// Faust UI description -> keysynth ParamSpecs (docs/PLUGINS.md "Parameters", ARCHITECTURE §10).
// Pure logic (no libfaust), so it is unit-tested without the Faust toolchain.
//
// Rules:
//  - id = [id:x] metadata if present, else snake_case of the path below the root group + label
//    ("Env/Attack" -> "env_attack"). Ids must match [a-z][a-z0-9_]*; duplicates are an error.
//  - group = innermost group below the root ("" at root); groups in order of first appearance -> uiHints.
//  - hslider/vslider/nentry: linear; [scale:log] -> Log; integral min/max/init with step >= 1 -> Int;
//    [style:menu{'A':0;'B':1}] / [style:radio{...}] -> Enum (value = index, mapped to the Faust value).
//  - checkbox / button -> Bool. hbargraph / vbargraph -> ReadOnly. [unit:x] -> unit, [hidden:1] -> Hidden.
//  - Instruments: controls labelled freq/key, gain/vel/velocity, gate are voice controls (driven per voice
//    by the host, not params).

#include "core/ParamSpec.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ks::plugins {

struct FaustControl {
    enum class Kind { Button, CheckBox, HSlider, VSlider, NumEntry, HBargraph, VBargraph };
    Kind kind = Kind::HSlider;
    std::string label;               // cleaned label (metadata stripped by the Faust compiler)
    std::vector<std::string> groups; // group labels below the root, outermost first
    float init = 0.0f, min = 0.0f, max = 1.0f, step = 0.0f;
    std::map<std::string, std::string> meta; // [key:value] metadata of this widget
};

struct FaustUiDesc {
    std::vector<FaustControl> controls; // in buildUserInterface order (= zone order)
    bool hasSoundfile = false;
};

enum class VoiceRole { None, Freq, Key, Gain, Velocity, Gate };

struct FaustBinding {
    int paramIndex = -1;            // index in params, -1 for voice controls
    VoiceRole role = VoiceRole::None;
    bool readOnly = false;
    std::vector<float> enumValues;  // Enum params: index -> Faust value
};

struct FaustMapping {
    std::vector<ParamSpec> params;
    std::vector<FaustBinding> bindings; // one per FaustUiDesc control (same order)
    std::vector<std::string> groupOrder;
    bool hasGate = false, hasFreq = false;
    std::string error; // non-empty: the plugin must not be registered
};

FaustMapping mapFaustUi(const FaustUiDesc& ui, bool instrument);

// "Cut-off Freq" -> "cut_off_freq". Empty if nothing usable remains.
std::string toSnakeCase(std::string_view s);
bool isValidParamId(std::string_view id);
// Parses "{'Saw':0;'Square':1}" (Faust menu/radio style). Returns false on syntax errors.
bool parseFaustMenu(std::string_view style, std::vector<std::string>& labels, std::vector<float>& values);

} // namespace ks::plugins
