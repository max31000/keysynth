#pragma once
// Topology-preserving-transform state-variable filter (Zavalishin / Simper "SVF"), 12 dB/oct.
// Stable under fast cutoff modulation. Outputs LP, BP, HP from one tick.

#include "dsp/Math.h"

namespace ks::dsp {

class Svf {
public:
    enum class Mode { LowPass = 0, BandPass = 1, HighPass = 2 };

    void setSampleRate(double sr) noexcept { sampleRate_ = static_cast<float>(sr); }

    // resonance 0..1 (1 = near self-oscillation).
    void setCutoff(float hz, float resonance) noexcept {
        const float f = std::fmin(std::fmax(hz, 10.0f), sampleRate_ * 0.49f);
        g_ = std::tan(kPi * f / sampleRate_);
        setGK(g_, dampingFor(resonance));
    }

    // k = 1/Q for resonance 0..1: 2 (Q=0.5) .. 0.02 (Q=50).
    static float dampingFor(float resonance) noexcept {
        return 2.0f - 1.98f * std::fmin(std::fmax(resonance, 0.0f), 1.0f);
    }
    // Raw coefficients: g = tan(pi fc / fs) (prewarped), k = damping. One division; cheap enough to call per
    // sample when the caller interpolates g (click-free fast cutoff modulation).
    void setGK(float g, float k) noexcept {
        g_ = g;
        k_ = k;
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }

    void reset() noexcept { ic1_ = ic2_ = 0.0f; }

    struct Out {
        float lp, bp, hp;
    };

    Out tick(float x) noexcept {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return {v2, v1, x - k_ * v1 - v2};
    }

    float process(float x, Mode m) noexcept {
        const Out o = tick(x);
        switch (m) {
        case Mode::BandPass: return o.bp;
        case Mode::HighPass: return o.hp;
        default: return o.lp;
        }
    }

private:
    float sampleRate_ = 48000.0f;
    float g_ = 0.1f, k_ = 1.4f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

} // namespace ks::dsp
