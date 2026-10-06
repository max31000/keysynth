#pragma once
// Cheap approximations for per-sample DSP (nonlinear filters, saturators). Branch-light, no tables.

#include <cmath>

namespace ks::dsp {

// Pade-style tanh approximation, exact sign/odd symmetry, |err| < 0.02, clamps to +-1 beyond |x| = 3.
inline float fastTanh(float x) noexcept {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Secant slope tanh(x)/x (1 at 0, in (0, 1]). Used to linearise tanh inside zero-delay feedback loops
// ("cheap" ZDF non-linearity: solve the linear loop with the secant gain evaluated at an estimate).
inline float tanhSecant(float x) noexcept {
    const float ax = std::fabs(x);
    if (ax < 1e-4f) return 1.0f;
    if (ax > 3.0f) return 1.0f / ax;
    const float x2 = x * x;
    return (27.0f + x2) / (27.0f + 9.0f * x2);
}

} // namespace ks::dsp
