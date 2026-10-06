#pragma once
// Topology-preserving (trapezoidal) one-pole filter: LP and HP outputs from one tick. Stable under fast
// cutoff modulation. Set the coefficient with G = g / (1 + g), g = tan(pi * fc / fs) (see tptG()).

#include "dsp/Math.h"

namespace ks::dsp {

// Prewarped one-pole gain for cutoff `hz` at sample rate `sr` (cutoff clamped to [1 Hz, 0.49 sr]).
inline float tptG(float hz, float sr) noexcept {
    const float f = std::fmin(std::fmax(hz, 1.0f), sr * 0.49f);
    const float g = std::tan(kPi * f / sr);
    return g / (1.0f + g);
}

class OnePoleTpt {
public:
    void reset(float v = 0.0f) noexcept { s_ = v; }
    void setG(float G) noexcept { G_ = G; }
    // Returns the low-pass output; high-pass = x - lp.
    float lp(float x) noexcept {
        const float v = (x - s_) * G_;
        const float y = v + s_;
        s_ = y + v;
        return y;
    }
    float hp(float x) noexcept { return x - lp(x); }

private:
    float G_ = 0.5f;
    float s_ = 0.0f;
};

} // namespace ks::dsp
