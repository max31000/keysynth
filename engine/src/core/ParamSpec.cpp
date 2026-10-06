#include "core/ParamSpec.h"

#include <algorithm>
#include <cmath>

namespace ks {

float ParamSpec::sanitize(float v) const noexcept {
    if (!std::isfinite(v)) v = def;
    v = std::clamp(v, min, max);
    switch (scale) {
    case ParamScale::Int:
    case ParamScale::Enum: v = std::round(v); break;
    case ParamScale::Bool: v = v >= 0.5f ? 1.0f : 0.0f; break;
    default: break;
    }
    return v;
}

namespace {
ParamSpec make(std::string id, std::string name, float min, float max, float def, ParamScale scale,
               std::string unit, std::string group) {
    ParamSpec p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.min = min;
    p.max = max;
    p.def = def;
    p.scale = scale;
    p.unit = std::move(unit);
    p.group = std::move(group);
    return p;
}
} // namespace

ParamSpec linearParam(std::string id, std::string name, float min, float max, float def, std::string unit,
                      std::string group) {
    return make(std::move(id), std::move(name), min, max, def, ParamScale::Linear, std::move(unit),
                std::move(group));
}

ParamSpec logParam(std::string id, std::string name, float min, float max, float def, std::string unit,
                   std::string group, float skewCentre) {
    auto p = make(std::move(id), std::move(name), min, max, def, ParamScale::Log, std::move(unit),
                  std::move(group));
    p.skewCentre = skewCentre;
    return p;
}

ParamSpec intParam(std::string id, std::string name, int min, int max, int def, std::string unit,
                   std::string group) {
    return make(std::move(id), std::move(name), static_cast<float>(min), static_cast<float>(max),
                static_cast<float>(def), ParamScale::Int, std::move(unit), std::move(group));
}

ParamSpec enumParam(std::string id, std::string name, std::vector<std::string> choices, int def,
                    std::string group) {
    const float maxIdx = static_cast<float>(choices.empty() ? 0 : choices.size() - 1);
    auto p = make(std::move(id), std::move(name), 0.0f, maxIdx, static_cast<float>(def), ParamScale::Enum, {},
                  std::move(group));
    p.choices = std::move(choices);
    return p;
}

ParamSpec boolParam(std::string id, std::string name, bool def, std::string group) {
    return make(std::move(id), std::move(name), 0.0f, 1.0f, def ? 1.0f : 0.0f, ParamScale::Bool, {},
                std::move(group));
}

const char* toString(ParamScale s) noexcept {
    switch (s) {
    case ParamScale::Linear: return "linear";
    case ParamScale::Log: return "log";
    case ParamScale::Int: return "int";
    case ParamScale::Enum: return "enum";
    case ParamScale::Bool: return "bool";
    }
    return "linear";
}

nlohmann::json toJson(const ParamSpec& p) {
    nlohmann::json j = {{"id", p.id},     {"name", p.name}, {"group", p.group},
                        {"unit", p.unit}, {"min", p.min},   {"max", p.max},
                        {"def", p.def},   {"scale", toString(p.scale)}};
    j["skewCentre"] = p.skewCentre;
    j["flags"] = p.flags; // bitmask: 1 ReadOnly, 2 Hidden, 4 NonAutomatable
    j["choices"] = p.choices;
    return j;
}

} // namespace ks
