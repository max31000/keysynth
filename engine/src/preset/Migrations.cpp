#include "preset/Migrations.h"

#include "preset/PatchJson.h"

namespace ks {

nlohmann::json migratePatchJson(const nlohmann::json& in, std::vector<std::string>* warnings) {
    nlohmann::json j = in;
    int format = kPatchFormat;
    if (auto it = j.find("format"); it != j.end() && it->is_number_integer()) format = it->get<int>();
    if (format > kPatchFormat && warnings)
        warnings->push_back("patch format " + std::to_string(format) + " is newer than supported (" +
                            std::to_string(kPatchFormat) + "); loading best-effort");
    // Migration steps go here, e.g.:
    //   if (format == 1) { /* rename va.cutoff_hz -> va.cutoff */ format = 2; }
    j["format"] = kPatchFormat;
    return j;
}

} // namespace ks
