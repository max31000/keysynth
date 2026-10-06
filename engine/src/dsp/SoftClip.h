#pragma once
// Soft clippers / saturators. Memoryless; no oversampling here (callers oversample if needed).

#include <cmath>

namespace ks::dsp {

// Cubic soft clip: identity-ish near 0, saturates to +-2/3 at |x| >= 1. Cheap.
inline float softClipCubic(float x) noexcept {
    if (x >= 1.0f) return 2.0f / 3.0f;
    if (x <= -1.0f) return -2.0f / 3.0f;
    return x - x * x * x / 3.0f;
}

// tanh saturation (exact).
inline float softClipTanh(float x) noexcept { return std::tanh(x); }

// Knee clipper: identity for |x| <= knee, then smoothly approaches `ceiling` (knee < ceiling).
// Continuous with slope 1 at the knee; output magnitude never exceeds ceiling.
inline float softClipKnee(float x, float knee, float ceiling) noexcept {
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float range = ceiling - knee;
    const float y = knee + range * std::tanh((a - knee) / range);
    return x < 0.0f ? -y : y;
}

} // namespace ks::dsp
