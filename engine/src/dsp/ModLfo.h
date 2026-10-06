#pragma once
// Modulation sources for effects: Lfo (sine / triangle, phase 0..1, multi-tap via phase offsets) and FastNoise
// (xorshift32 white noise, -1..1). RT-safe, no allocation.

#include "dsp/Math.h"

#include <cmath>
#include <cstdint>

namespace ks::dsp {

class FastNoise {
public:
    explicit FastNoise(uint32_t seed = 0x9E3779B9u) : s_(seed ? seed : 1u) {}
    void seed(uint32_t s) noexcept { s_ = s ? s : 1u; }
    float next() noexcept {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return static_cast<float>(static_cast<int32_t>(s_)) * (1.0f / 2147483648.0f);
    }

private:
    uint32_t s_;
};

class Lfo {
public:
    enum class Shape { Sine, Triangle };
    void setSampleRate(double sr) noexcept { invSr_ = 1.0f / static_cast<float>(sr); }
    void setRate(float hz) noexcept { inc_ = std::fmax(hz, 0.0f) * invSr_; }
    void setPhase(float p) noexcept { phase_ = p - std::floor(p); }
    float phase() const noexcept { return phase_; }
    // Value at phase + offset (0..1), without advancing. Sine: sin(2 pi p). Triangle: -1 at 0, +1 at 0.5.
    float valueAt(Shape s, float offset) const noexcept {
        float p = phase_ + offset;
        p -= std::floor(p);
        if (s == Shape::Sine) return std::sin(kTwoPi * p);
        return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
    }
    void advance(int n = 1) noexcept {
        phase_ += inc_ * static_cast<float>(n);
        phase_ -= std::floor(phase_);
    }

private:
    float invSr_ = 1.0f / 48000.0f;
    float inc_ = 0.0f;
    float phase_ = 0.0f;
};

} // namespace ks::dsp
