#pragma once
// Drum pattern data (ARCHITECTURE §8, schema docs/PRESETS.md "Patterns").
//   Pattern   : control-thread representation (strings, vectors), JSON <-> struct, validation.
//   RtPattern : fixed-size POD copied into the DrumSequencer's triple buffer (audio thread reads it).

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ks {

inline constexpr int kPatternMaxTracks = 16;
inline constexpr int kPatternMaxSteps = 256;
inline constexpr int kPatternMaxBars = 4;

struct PatternTrack {
    std::string name;
    int note = 36;
    bool mute = false;
    std::vector<uint8_t> steps; // velocity 0 = off, 1..127
};

struct Pattern {
    std::string name = "Empty";
    std::string description;
    double tempo = 0.0; // 0 = not specified
    int numerator = 4, denominator = 4;
    int stepsPerBeat = 4;
    int bars = 1;
    float swing = 0.0f;        // 0..1
    float accentAmount = 0.5f; // 0..1 -> +accentAmount*64 velocity
    int kit = -1;              // -1 = not specified, else drums `kit` enum index
    std::vector<PatternTrack> tracks;
    std::vector<uint8_t> accent; // 0/1 per step

    int stepsPerBar() const noexcept { return numerator * stepsPerBeat; }
    int numSteps() const noexcept { return bars * stepsPerBar(); }
    // Quarter notes per step / per bar.
    double stepPpq() const noexcept { return 4.0 / (static_cast<double>(denominator) * stepsPerBeat); }
    double barPpq() const noexcept { return 4.0 * numerator / static_cast<double>(denominator); }

    // Clamp everything into range and resize every row to numSteps(). Returns warnings.
    std::vector<std::string> normalize();
    // Change the meter, keeping steps by index (bars kept; reduced if the result exceeds kPatternMaxSteps).
    void setTimeSignature(int num, int den);
};

// Lenient parse (unknown keys ignored with a warning, bad values clamped). Throws std::invalid_argument only for
// a non-object.
Pattern patternFromJson(const nlohmann::json& j, std::vector<std::string>* warnings = nullptr);
// Canonical form (steps/accent as number arrays).
nlohmann::json patternToJson(const Pattern& p);
// Kit names used in pattern files ("808", "909", "linn", "industrial") -> drums `kit` index, -1 if unknown.
int kitIndexFromName(const std::string& s);

// Audio-thread view. Trivially copyable, fixed size (~4.4 KB).
struct RtPattern {
    struct Track {
        uint8_t note = 36;
        bool mute = false;
        std::array<uint8_t, kPatternMaxSteps> vel{}; // already includes accent
    };
    int numTracks = 0;
    int numSteps = 16;
    int numerator = 4, denominator = 4, stepsPerBeat = 4;
    std::array<Track, kPatternMaxTracks> tracks{};

    double stepPpq() const noexcept { return 4.0 / (static_cast<double>(denominator) * stepsPerBeat); }
    int stepsPerBar() const noexcept { return numerator * stepsPerBeat; }
};

RtPattern toRt(const Pattern& p);

} // namespace ks
