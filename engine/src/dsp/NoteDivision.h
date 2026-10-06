#pragma once
// Tempo-synced note divisions shared by effects (`sync` enum params). Index 0 = "Off" (free-running in ms/Hz).
// Lengths in quarter-note beats; "." = dotted (x1.5), "T" = triplet (x2/3).

#include <array>
#include <string>
#include <vector>

namespace ks::dsp {

struct NoteDivision {
    const char* label;
    double beats; // quarter notes
};

inline constexpr std::array<NoteDivision, 18> kNoteDivisions{{
    {"Off", 0.0},
    {"2/1", 8.0},    {"1/1", 4.0},     {"1/2", 2.0},     {"1/2.", 3.0},  {"1/2T", 4.0 / 3.0},
    {"1/4", 1.0},    {"1/4.", 1.5},    {"1/4T", 2.0 / 3.0},
    {"1/8", 0.5},    {"1/8.", 0.75},   {"1/8T", 1.0 / 3.0},
    {"1/16", 0.25},  {"1/16.", 0.375}, {"1/16T", 1.0 / 6.0},
    {"1/32", 0.125}, {"1/32.", 0.1875}, {"1/32T", 1.0 / 12.0},
}};

// Enum choices for a `sync` param (control thread).
inline std::vector<std::string> noteDivisionChoices() {
    std::vector<std::string> v;
    for (const auto& d : kNoteDivisions) v.emplace_back(d.label);
    return v;
}

// Beats of a sync index (0 for Off / out of range).
inline double noteDivisionBeats(int index) noexcept {
    if (index <= 0 || index >= static_cast<int>(kNoteDivisions.size())) return 0.0;
    return kNoteDivisions[static_cast<size_t>(index)].beats;
}

// Division length in seconds at `bpm` (0 for Off). Non-positive tempo falls back to 120.
inline double noteDivisionSeconds(int index, double bpm) noexcept {
    const double b = noteDivisionBeats(index);
    if (b <= 0.0) return 0.0;
    const double t = bpm > 1.0 ? bpm : 120.0;
    return b * 60.0 / t;
}

} // namespace ks::dsp
