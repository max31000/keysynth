#pragma once
// Bank of N exponentially decaying resonators for modal synthesis.
// Each mode is a complex one-pole  z <- p * z + g * x,  p = r * e^{jw}  (exact frequency, no tuning warp at high
// w, cheap to re-damp: scaling r keeps the phase). Mode displacement = Im(z), so an impulse produces
// g * r^n * sin(w n) (starts at zero, like a struck bar). Two weighted output buses per mode (e.g. "through the
// pickup" and "acoustic/mechanical"). RT-safe, no allocation; unused modes have g = r = 0.

#include "dsp/Math.h"

#include <array>
#include <cmath>

namespace ks::dsp {

template <int N>
class ModalBank {
    static_assert(N > 0 && N <= 64);

public:
    struct Out {
        float a = 0.0f; // bus A
        float b = 0.0f; // bus B
    };

    static constexpr int size() noexcept { return N; }

    // Pole radius for a 60 dB decay time (seconds). t60 <= 0 -> 0 (mode off).
    static float radiusForT60(double t60, double sampleRate) noexcept {
        if (t60 <= 0.0 || sampleRate <= 0.0) return 0.0f;
        return static_cast<float>(std::exp(-6.907755278982137 / (t60 * sampleRate)));
    }

    void clear() noexcept {
        zr_.fill(0.0f);
        zi_.fill(0.0f);
    }

    // Full reset: state and coefficients.
    void reset() noexcept {
        clear();
        pr_.fill(0.0f);
        pi_.fill(0.0f);
        cw_.fill(1.0);
        sw_.fill(0.0);
        r_.fill(0.0f);
        w_.fill(0.0f);
        g_.fill(0.0f);
        wa_.fill(0.0f);
        wb_.fill(0.0f);
    }

    // omega = 2*pi*f/sr (radians/sample, must be < pi), r = pole radius (0..1).
    void setMode(int i, float omega, float r, float inGain, float weightA, float weightB) noexcept {
        const auto k = static_cast<size_t>(i);
        w_[k] = omega;
        r_[k] = r;
        g_[k] = inGain;
        wa_[k] = weightA;
        wb_[k] = weightB;
        updatePole(k);
    }
    void setFrequency(int i, float omega) noexcept {
        const auto k = static_cast<size_t>(i);
        if (w_[k] == omega) return;
        w_[k] = omega;
        updatePole(k);
    }
    // Re-damp without trig (cached cos/sin of w; no drift of |p| over many updates), keeps the phase.
    void setRadius(int i, float r) noexcept {
        const auto k = static_cast<size_t>(i);
        r_[k] = r;
        pr_[k] = static_cast<float>(static_cast<double>(r) * cw_[k]);
        pi_[k] = static_cast<float>(static_cast<double>(r) * sw_[k]);
    }
    void setInputGain(int i, float g) noexcept { g_[static_cast<size_t>(i)] = g; }

    // Current bus-A output (sum of Im(z) * weightA) without advancing the state.
    float busA() const noexcept {
        float a = 0.0f;
        for (size_t k = 0; k < static_cast<size_t>(N); ++k) a += zi_[k] * wa_[k];
        return a;
    }
    float radius(int i) const noexcept { return r_[static_cast<size_t>(i)]; }
    float omega(int i) const noexcept { return w_[static_cast<size_t>(i)]; }

    Out tick(float x) noexcept {
        Out o;
        for (size_t k = 0; k < static_cast<size_t>(N); ++k) {
            const float zr = zr_[k], zi = zi_[k];
            const float nr = pr_[k] * zr - pi_[k] * zi + g_[k] * x;
            const float ni = pr_[k] * zi + pi_[k] * zr;
            zr_[k] = nr;
            zi_[k] = ni;
            o.a += ni * wa_[k];
            o.b += ni * wb_[k];
        }
        return o;
    }

    // Sum of squared mode amplitudes, weighted by the larger bus weight (silence detection).
    float energy() const noexcept {
        float e = 0.0f;
        for (size_t k = 0; k < static_cast<size_t>(N); ++k) {
            const float w = std::fmax(std::fabs(wa_[k]), std::fabs(wb_[k]));
            e += (zr_[k] * zr_[k] + zi_[k] * zi_[k]) * w * w;
        }
        return e;
    }

private:
    void updatePole(size_t k) noexcept {
        const double w = w_[k];
        cw_[k] = std::cos(w);
        sw_[k] = std::sin(w);
        pr_[k] = static_cast<float>(r_[k] * cw_[k]);
        pi_[k] = static_cast<float>(r_[k] * sw_[k]);
    }

    std::array<float, N> zr_{}, zi_{}, pr_{}, pi_{}, r_{}, w_{}, g_{}, wa_{}, wb_{};
    std::array<double, N> cw_{}, sw_{};
};

// Hammer force pulse f(t) = (t/tau) e^{1 - t/tau} ("alpha" pulse: smooth onset, peak at tau, felt-like tail).
// Integral = e*tau; spectrum magnitude normalized to 1 at DC = 1 / (1 + (2 pi f tau)^2): monotonic in f and tau
// (no notches, unlike a half-sine), so a softer/longer contact always means fewer high modes.
inline float alphaPulseSpectrum(float f, float tau) noexcept {
    const float a = kTwoPi * f * tau;
    return 1.0f / (1.0f + a * a);
}

} // namespace ks::dsp
