#pragma once
// Patch format migrations (docs/PRESETS.md). `format` is bumped only for breaking changes; each step
// N -> N+1 lives in Migrations.cpp. Param id renames also go here (they are compatibility surfaces).

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace ks {

// Returns a copy migrated to the current format. Unknown future formats are loaded best-effort with a warning.
nlohmann::json migratePatchJson(const nlohmann::json& in, std::vector<std::string>* warnings);

} // namespace ks
