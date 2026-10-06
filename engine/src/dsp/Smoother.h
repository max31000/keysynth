#pragma once
// Parameter smoothers. RT-safe, no allocation.
//   OnePoleSmoother: exponential approach (good for gains/cutoffs; never quite settles, snaps below epsilon).
//   LinearSmoother:  fixed-duration linear ramp to each new target (good for crossfades).

#include "dsp/Math.h"

namespace ks::dsp {

class OnePoleSmoother {
public:
    void prepare(double sampleRate, float timeSeconds) noexcept {
        sampleRate_ = sampleRate;
        setTime(timeSeconds);
    }
    void setTime(float timeSeconds) noexcept { coef_ = onePoleCoef(timeSeconds, sampleRate_); }
    void setTarget(float t) noexcept { target_ = t; }
    void snap(float v) noexcept { value_ = target_ = v; }
    float next() noexcept {
        const float nv = value_ + (target_ - value_) * coef_;
        // Snap when close or when float precision stalls the approach.
        value_ = (nv == value_ || std::fabs(target_ - nv) < 1e-6f) ? target_ : nv;
        return value_;
    }
    float value() const noexcept { return value_; }
    float target() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return value_ != target_; }

private:
    double sampleRate_ = 48000.0;
    float coef_ = 1.0f;
    float value_ = 0.0f;
    float target_ = 0.0f;
};

class LinearSmoother {
public:
    void prepare(double sampleRate, float rampSeconds) noexcept {
        rampSamples_ = static_cast<int>(rampSeconds * static_cast<float>(sampleRate));
        if (rampSamples_ < 1) rampSamples_ = 1;
    }
    void setTarget(float t) noexcept {
        if (t == target_) return;
        target_ = t;
        step_ = (target_ - value_) / static_cast<float>(rampSamples_);
        remaining_ = rampSamples_;
    }
    void snap(float v) noexcept {
        value_ = target_ = v;
        remaining_ = 0;
    }
    float next() noexcept {
        if (remaining_ > 0) {
            value_ += step_;
            if (--remaining_ == 0) value_ = target_;
        }
        return value_;
    }
    float value() const noexcept { return value_; }
    bool isSmoothing() const noexcept { return remaining_ > 0; }

private:
    int rampSamples_ = 1;
    int remaining_ = 0;
    float value_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 0.0f;
};

} // namespace ks::dsp
