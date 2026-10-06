#include "preset/Migrations.h"

#include "dsp/NoteDivision.h"
#include "preset/PatchJson.h"

#include <cmath>
#include <functional>

namespace ks {

namespace {

using nlohmann::json;

// Calls fn(type, params) for every module slot with an object `params` (layer instruments, layer fx, master fx).
void forEachSlot(json& j, const std::function<void(const std::string&, json&)>& fn) {
    auto visit = [&](json& slot) {
        if (!slot.is_object()) return;
        auto t = slot.find("type");
        auto p = slot.find("params");
        if (t == slot.end() || !t->is_string() || p == slot.end() || !p->is_object()) return;
        fn(t->get<std::string>(), *p);
    };
    if (auto ls = j.find("layers"); ls != j.end() && ls->is_array())
        for (auto& l : *ls) {
            if (!l.is_object()) continue;
            if (auto in = l.find("instrument"); in != l.end()) visit(*in);
            if (auto fx = l.find("fx"); fx != l.end() && fx->is_array())
                for (auto& f : *fx) visit(f);
        }
    if (auto m = j.find("master"); m != j.end() && m->is_object())
        if (auto fx = m->find("fx"); fx != m->end() && fx->is_array())
            for (auto& f : *fx) visit(f);
}

// Numeric (or boolean) param value, or `def` when missing / wrong-typed.
double num(const json& params, const std::string& id, double def) {
    auto it = params.find(id);
    if (it == params.end()) return def;
    if (it->is_number()) return it->get<double>();
    if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
    return def;
}

// Old per-module division lists (format 1) -> label, mapped onto dsp::kNoteDivisions by name.
constexpr const char* kOldTremoloDivisions[] = {"1/1", "1/2",  "1/2T", "1/4.",  "1/4",   "1/4T",
                                                "1/8.", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
constexpr int kOldTremoloDefault = 7; // 1/8
constexpr const char* kOldVaDivisions[] = {"1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D",
                                           "1/4",  "1/2T",  "1/4D", "1/2",  "1/1",   "2/1", "4/1"};
constexpr int kOldVaDefault = 8; // 1/4

template <size_t N>
int mapOldDivision(const char* const (&labels)[N], double oldIndex, int oldDefault) {
    long i = std::isfinite(oldIndex) ? std::lround(oldIndex) : -1;
    if (i < 0 || i >= static_cast<long>(N)) i = oldDefault;
    const int idx = dsp::noteDivisionIndex(labels[i]); // every old label exists (test "Migration: ...")
    return idx > 0 ? idx : 0;
}

// Format-1 "sync bool + division enum" pair -> one `sync` NoteDivision enum (0 = Off).
template <size_t N>
void mergeSyncDivision(json& params, const std::string& syncId, const std::string& divId,
                       const char* const (&labels)[N], int oldDefault) {
    const bool on = num(params, syncId, 0.0) >= 0.5;
    const double div = num(params, divId, oldDefault);
    params[syncId] = on ? mapOldDivision(labels, div, oldDefault) : 0;
    params.erase(divId);
}

// In a current-format patch, an old `division` id only means the old pair if `sync` still looks like the old
// bool (true/false/0/1); otherwise `sync` is already a NoteDivision index and the stray `division` is dropped.
bool isOldPair(const json& params, const std::string& syncId, const std::string& divId) {
    if (!params.contains(divId)) return false;
    auto it = params.find(syncId);
    if (it == params.end() || it->is_boolean()) return true;
    return it->is_number() && (it->get<double>() == 0.0 || it->get<double>() == 1.0);
}

// Format 1 -> 2: one shared note-division enum for every tempo-syncable param (ARCHITECTURE §7):
//  - delay / phaser / flanger `sync`: "4/1" was inserted at index 1, so indices >= 1 shift by one;
//  - tremolo `sync` (bool) + `division` and va `lfoN_sync` (bool) + `lfoN_division` (own orders) merge into
//    `sync` / `lfoN_sync` enums.
// The merge also runs on a newer-format patch that still carries the old `division` ids (hand-edited files).
void migrateSyncDivisions(json& j, int format) {
    forEachSlot(j, [&](const std::string& type, json& params) {
        if (type == "delay" || type == "phaser" || type == "flanger") {
            if (format >= 2) return;
            if (auto it = params.find("sync"); it != params.end() && it->is_number()) {
                const double v = it->get<double>();
                if (std::isfinite(v) && std::lround(v) >= 1) *it = std::lround(v) + 1;
            }
        } else if (type == "tremolo") {
            if (format < 2 || isOldPair(params, "sync", "division"))
                mergeSyncDivision(params, "sync", "division", kOldTremoloDivisions, kOldTremoloDefault);
            else
                params.erase("division");
        } else if (type == "va") {
            for (const char* n : {"lfo1_", "lfo2_"}) {
                const std::string sync = std::string(n) + "sync", div = std::string(n) + "division";
                if (!params.contains(sync) && !params.contains(div)) continue; // defaults: nothing to merge
                if (format < 2 || isOldPair(params, sync, div))
                    mergeSyncDivision(params, sync, div, kOldVaDivisions, kOldVaDefault);
                else
                    params.erase(div);
            }
        }
    });
}

} // namespace

nlohmann::json migratePatchJson(const nlohmann::json& in, std::vector<std::string>* warnings) {
    nlohmann::json j = in;
    int format = kPatchFormat;
    if (auto it = j.find("format"); it != j.end() && it->is_number_integer()) format = it->get<int>();
    if (format > kPatchFormat && warnings)
        warnings->push_back("patch format " + std::to_string(format) + " is newer than supported (" +
                            std::to_string(kPatchFormat) + "); loading best-effort");
    // 1 -> 2: unified note-division `sync` enums (old ids are also merged when present in any format).
    migrateSyncDivisions(j, format);
    j["format"] = kPatchFormat;
    return j;
}

} // namespace ks
