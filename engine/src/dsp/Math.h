#pragma once
// Small numeric helpers shared by DSP code.

#include <cmath>
#include <numbers>

namespace ks::dsp {

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

inline float dbToGain(float db) noexcept { return db <= -120.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) noexcept { return g <= 1e-6f ? -120.0f : 20.0f * std::log10(g); }

// MIDI note (fractional) -> Hz, A4 = 440 Hz.
inline float noteToHz(float note) noexcept { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

// Equal-power pan, pan in -1..1 -> left/right gains (centre = -3 dB each).
inline void panGains(float pan, float& l, float& r) noexcept {
    const float a = (std::fmin(std::fmax(pan, -1.0f), 1.0f) + 1.0f) * 0.25f * kPi;
    l = std::cos(a);
    r = std::sin(a);
}

// One-pole coefficient for a time constant (time to reach ~63%).
inline float onePoleCoef(float timeSeconds, double sampleRate) noexcept {
    if (timeSeconds <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-1.0f / (timeSeconds * static_cast<float>(sampleRate)));
}

} // namespace ks::dsp
