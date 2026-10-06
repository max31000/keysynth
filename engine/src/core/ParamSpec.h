#pragma once
// Parameter metadata (ARCHITECTURE §5.1). Param ids are part of the preset format: renaming one needs a
// migration in preset/Migrations.cpp.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ks {

enum class ParamScale { Linear, Log, Int, Enum, Bool };

enum ParamFlags : uint32_t {
    None = 0,
    ReadOnly = 1,       // written by the audio thread (meters/readouts), polled into telemetry
    Hidden = 2,         // not shown by the generic UI
    NonAutomatable = 4, // structural-ish; UI may still edit it
};

struct ParamSpec {
    std::string id;   // stable snake_case, unique within the module
    std::string name; // display name
    std::string group;
    std::string unit; // "dB", "Hz", "s", "%", "st", ...
    float min = 0.0f;
    float max = 1.0f;
    float def = 0.0f;
    ParamScale scale = ParamScale::Linear;
    float skewCentre = 0.0f; // Log params: value at knob centre (0 = geometric mean of min/max)
    uint32_t flags = ParamFlags::None;
    std::vector<std::string> choices; // Enum labels; value = index

    // Clamp (and round for Int/Enum/Bool) a plain value into the spec range. NaN -> default.
    float sanitize(float v) const noexcept;
    bool isReadOnly() const noexcept { return (flags & ParamFlags::ReadOnly) != 0; }
};

// Builders for concise static ModuleInfo tables.
ParamSpec linearParam(std::string id, std::string name, float min, float max, float def, std::string unit = {},
                      std::string group = {});
ParamSpec logParam(std::string id, std::string name, float min, float max, float def, std::string unit = {},
                   std::string group = {}, float skewCentre = 0.0f);
ParamSpec intParam(std::string id, std::string name, int min, int max, int def, std::string unit = {},
                   std::string group = {});
ParamSpec enumParam(std::string id, std::string name, std::vector<std::string> choices, int def,
                    std::string group = {});
ParamSpec boolParam(std::string id, std::string name, bool def, std::string group = {});

const char* toString(ParamScale s) noexcept;
nlohmann::json toJson(const ParamSpec& p);

} // namespace ks
