#pragma once
// LFO helpers: stateless shape of a running phase (in cycles, integer part = cycle count, used for sample &
// hold), delay/fade-in curve (tempo divisions: dsp/NoteDivision.h). Callers own the phase (per voice for key sync, global
// otherwise), which keeps per-voice LFOs to one double each.

#include "dsp/Math.h"
#include "dsp/Noise.h"

#include <cmath>
#include <cstdint>

namespace ks::dsp {

enum class LfoWave { Sine = 0, Triangle = 1, SawUp = 2, SawDown = 3, Square = 4, SampleHold = 5 };

// Bipolar -1..1. Triangle/sine start at 0 rising; S&H is a hash of the cycle index (deterministic per seed).
inline float lfoShape(LfoWave w, double cycles, uint32_t seed) noexcept {
    const double fl = std::floor(cycles);
    const float f = static_cast<float>(cycles - fl);
    switch (w) {
    case LfoWave::Sine: return std::sin(kTwoPi * f);
    case LfoWave::Triangle: return f < 0.25f ? 4.0f * f : (f < 0.75f ? 2.0f - 4.0f * f : 4.0f * f - 4.0f);
    case LfoWave::SawUp: return 2.0f * f - 1.0f;
    case LfoWave::SawDown: return 1.0f - 2.0f * f;
    case LfoWave::Square: return f < 0.5f ? 1.0f : -1.0f;
    case LfoWave::SampleHold:
        return bipolarFromBits(hash32(static_cast<uint32_t>(static_cast<int64_t>(fl)) * 0x9e3779b9u ^ seed));
    }
    return 0.0f;
}

// Juno-style delay: silent for the first half of `delay`, then a linear fade-in over the second half.
inline float lfoDelayGain(float timeSinceNoteOn, float delay) noexcept {
    if (delay <= 1e-4f) return 1.0f;
    const float x = (timeSinceNoteOn - 0.5f * delay) / (0.5f * delay);
    return x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : x);
}

} // namespace ks::dsp
