#pragma once
// Mild tube-style preamp stage: asymmetric tanh (bias shifts the operating point -> even harmonics), gain
// half-normalised (small-signal gain = sqrt(drive gain)), followed by a 10 Hz DC blocker (the bias
// produces DC). Drive 0 = nearly linear (soft limit only near full scale). Not oversampled: keep drive mild.

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ks::dsp {

class TubeStage {
public:
    void prepare(double sampleRate) noexcept {
        dcCoef_ = 1.0f - kTwoPi * 10.0f / static_cast<float>(sampleRate);
        reset();
    }
    void reset() noexcept { dcX_ = dcY_ = 0.0f; }

    // drive 0..1. Call per block (cheap) or per sample.
    void setDrive(float drive) noexcept {
        const float d = std::clamp(drive, 0.0f, 1.0f);
        gain_ = 1.0f + 9.0f * d * d + 2.0f * d;
        bias_ = 0.25f * d;
        const float tb = std::tanh(bias_);
        offset_ = tb;
        // Small-signal slope of tanh(g x + b) at x = 0 is g (1 - tanh^2 b). Normalise only half-way (in dB):
        // more drive = a bit louder for quiet input, compressed/saturated for loud input, like a real preamp.
        norm_ = 1.0f / (std::sqrt(gain_) * (1.0f - tb * tb));
    }

    float process(float x) noexcept {
        const float y = (std::tanh(gain_ * x + bias_) - offset_) * norm_;
        // DC blocker: y[n] = x[n] - x[n-1] + R y[n-1]
        const float out = y - dcX_ + dcCoef_ * dcY_;
        dcX_ = y;
        dcY_ = out;
        return out;
    }

private:
    float gain_ = 1.0f, bias_ = 0.0f, offset_ = 0.0f, norm_ = 1.0f;
    float dcCoef_ = 0.999f, dcX_ = 0.0f, dcY_ = 0.0f;
};

// xorshift32 white noise in [-1, 1). Deterministic, RT-safe.
class XorNoise {
public:
    explicit XorNoise(uint32_t seed = 0x9E3779B9u) noexcept : s_(seed ? seed : 1u) {}
    void seed(uint32_t s) noexcept { s_ = s ? s : 1u; }
    uint32_t nextU32() noexcept {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }
    float next() noexcept { return static_cast<float>(static_cast<int32_t>(nextU32())) * (1.0f / 2147483648.0f); }

private:
    uint32_t s_;
};

} // namespace ks::dsp
