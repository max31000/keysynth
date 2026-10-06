#pragma once
// Patch <-> JSON (docs/PRESETS.md). Loading is lenient: wrong-typed or unknown keys are ignored with a warning;
// missing values take defaults. Param normalization against module specs happens in PatchModel::setPatch.

#include "core/Patch.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace ks {

inline constexpr int kPatchFormat = 2; // 2: unified note-division `sync` enums (Migrations.cpp)

Patch patchFromJson(const nlohmann::json& j, std::vector<std::string>* warnings = nullptr);
nlohmann::json patchToJson(const Patch& p);

Zone zoneFromJson(const nlohmann::json& j, const Zone& base = {}, std::vector<std::string>* warnings = nullptr);
nlohmann::json zoneToJson(const Zone& z);
ModuleSlot slotFromJson(const nlohmann::json& j, std::vector<std::string>* warnings, const std::string& where);
nlohmann::json slotToJson(const ModuleSlot& s, bool isFx);

} // namespace ks
