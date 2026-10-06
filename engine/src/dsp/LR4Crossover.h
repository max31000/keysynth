#pragma once
// Linkwitz-Riley 4th-order (24 dB/oct) two-way crossover: each band = two cascaded 2nd-order Butterworth TPT
// SVF sections (Q = 1/sqrt2). low + high sums to an allpass (flat magnitude). RT-safe.

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks::dsp {

class LR4Crossover {
public:
    void prepare(double sampleRate, float freqHz) noexcept {
        sampleRate_ = static_cast<float>(sampleRate);
        setFrequency(freqHz);
        reset();
    }
    void setFrequency(float hz) noexcept {
        const float f = std::clamp(hz, 10.0f, sampleRate_ * 0.45f);
        g_ = std::tan(kPi * f / sampleRate_);
        a1_ = 1.0f / (1.0f + g_ * (g_ + kK));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }
    void reset() noexcept {
        for (auto& s : st_) s = {};
    }

    void process(float x, float& low, float& high) noexcept {
        low = tick(st_[1], tick(st_[0], x).lp).lp;
        high = tick(st_[3], tick(st_[2], x).hp).hp;
    }

private:
    static constexpr float kK = 1.41421356f; // 1/Q for Butterworth
    struct State {
        float ic1 = 0.0f, ic2 = 0.0f;
    };
    struct Out {
        float lp, hp;
    };
    Out tick(State& s, float x) const noexcept {
        const float v3 = x - s.ic2;
        const float v1 = a1_ * s.ic1 + a2_ * v3;
        const float v2 = s.ic2 + a2_ * s.ic1 + a3_ * v3;
        s.ic1 = 2.0f * v1 - s.ic1;
        s.ic2 = 2.0f * v2 - s.ic2;
        return {v2, x - kK * v1 - v2};
    }

    float sampleRate_ = 48000.0f;
    float g_ = 0.05f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    State st_[4];
};

} // namespace ks::dsp
