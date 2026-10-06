#pragma once
// Euler-Bernoulli clamped-free (cantilever) beam modes: Rhodes tines, Wurlitzer reeds, kalimba tines...
// Control-rate helpers (double precision; call at note-on, not per sample).

#include <cmath>

namespace ks::dsp::beam {

inline constexpr int kModes = 4;
// beta_k * L for the first modes of a clamped-free beam, and sigma_k = (cosh b + cos b)/(sinh b + sin b).
inline constexpr double kBeta[kModes] = {1.8751040687, 4.6940911330, 7.8547574382, 10.9955407349};
inline constexpr double kSigma[kModes] = {0.7340955137, 1.0184673165, 0.9992244965, 1.0000335508};

// Frequency ratio of mode k (0-based) to the fundamental: (beta_k / beta_0)^2 = 1, 6.267, 17.547, 34.386.
inline double ratio(int k) noexcept {
    const double b = kBeta[k] / kBeta[0];
    return b * b;
}

// Mode shape phi_k(xi), xi = position from the clamp (0) to the free tip (1). |phi_k(1)| = 2.
inline double shape(int k, double xi) noexcept {
    const double b = kBeta[k] * xi;
    return std::cosh(b) - std::cos(b) - kSigma[k] * (std::sinh(b) - std::sin(b));
}

// Tip displacement of mode k relative to mode 0 for an impulsive force at xi, including the 1/omega_k of a
// velocity impulse: phi_k(xi) phi_k(1) / (phi_0(xi) phi_0(1)) / ratio(k). Signed.
inline double struckTipAmplitude(int k, double xi) noexcept {
    const double p0 = shape(0, xi) * shape(0, 1.0);
    if (std::fabs(p0) < 1e-12) return 0.0;
    return shape(k, xi) * shape(k, 1.0) / p0 / ratio(k);
}

} // namespace ks::dsp::beam
