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
    void setTime(float timeSeconds) noexcept {
        coef_ = timeSeconds <= 0.0f ? 1.0 : 1.0 - std::exp(-1.0 / (static_cast<double>(timeSeconds) * sampleRate_));
    }
    void setTarget(float t) noexcept { target_ = t; }
    void snap(float v) noexcept { value_ = target_ = v; }
    // State in double: a float approach stalls short of large targets (delay times of thousands of samples: the
    // step falls below one ulp several samples away) and the snap then jumps (click). Double keeps converging.
    float next() noexcept {
        const double nv = value_ + (static_cast<double>(target_) - value_) * coef_;
        value_ = (nv == value_ || std::fabs(static_cast<double>(target_) - nv) < 1e-6) ? static_cast<double>(target_) : nv;
        return static_cast<float>(value_);
    }
    float value() const noexcept { return static_cast<float>(value_); }
    float target() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return value_ != static_cast<double>(target_); }

private:
    double sampleRate_ = 48000.0;
    double coef_ = 1.0;
    double value_ = 0.0;
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
