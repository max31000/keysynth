#pragma once
// Zero-lookahead safety limiter (ARCHITECTURE §4.7): stereo-linked instant-attack gain reduction towards a
// threshold, fast exponential release, followed by a knee soft clipper that guarantees |out| < ceiling.
// Adds no latency. Not transparent when driven hard — it is a safety net, not a mastering limiter.

#include "dsp/Math.h"
#include "dsp/SoftClip.h"

namespace ks::dsp {

class SafetyLimiter {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate;
        setRelease(releaseMs_);
        reset();
    }
    // ceilingDb <= 0. Gain reduction starts at the ceiling; the knee clipper starts 1.5 dB below it.
    void setCeilingDb(float db) noexcept {
        ceiling_ = dbToGain(std::fmin(db, 0.0f)) * 0.999f;
        knee_ = ceiling_ * 0.84f;
    }
    void setRelease(float ms) noexcept {
        releaseMs_ = ms;
        releaseCoef_ = onePoleCoef(std::fmax(ms, 1.0f) * 0.001f, sampleRate_);
    }
    void reset() noexcept { gain_ = 1.0f; }
    float gainReductionDb() const noexcept { return -gainToDb(gain_); }

    void process(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i])) { // never pass NaN/Inf to the device
                l[i] = r[i] = 0.0f;
                continue;
            }
            const float a = std::fmax(std::fabs(l[i]), std::fabs(r[i]));
            const float target = a > ceiling_ ? ceiling_ / a : 1.0f;
            if (target < gain_) gain_ = target;                 // instant attack
            else gain_ += (target - gain_) * releaseCoef_;      // release
            l[i] = softClipKnee(l[i] * gain_, knee_, ceiling_);
            r[i] = softClipKnee(r[i] * gain_, knee_, ceiling_);
        }
    }

private:
    double sampleRate_ = 48000.0;
    float releaseMs_ = 60.0f;
    float releaseCoef_ = 0.001f;
    float ceiling_ = 0.98f;
    float knee_ = 0.82f;
    float gain_ = 1.0f;
};

} // namespace ks::dsp
