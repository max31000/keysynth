#pragma once
// Attack/release envelope follower (rectified input through a one-pole whose coefficient depends on the direction).
// Used by `vocoder` (one per band) and `wah` (auto-wah). RT-safe, no allocation.
//   setTimes(attackSeconds, releaseSeconds, sampleRate) — time constants to ~63 %.
//   process(x): |x| -> envelope (peak-ish; feed x*x and take sqrt outside for an RMS-like follower).

#include "dsp/Math.h"

#include <cmath>

namespace ks::dsp {

class EnvelopeFollower {
public:
    void setTimes(float attackSeconds, float releaseSeconds, double sampleRate) noexcept {
        att_ = onePoleCoef(attackSeconds, sampleRate);
        rel_ = onePoleCoef(releaseSeconds, sampleRate);
    }
    void reset(float v = 0.0f) noexcept { env_ = v; }
    // Follows a non-negative level (|x|, x^2, ...).
    float follow(float level) noexcept {
        env_ += (level - env_) * (level > env_ ? att_ : rel_);
        if (!(env_ > 1e-15f)) env_ = 0.0f; // flush tiny values (and NaN) so silence costs nothing
        return env_;
    }
    float process(float x) noexcept { return follow(std::fabs(x)); }
    float value() const noexcept { return env_; }

private:
    float att_ = 1.0f, rel_ = 1.0f;
    float env_ = 0.0f;
};

} // namespace ks::dsp
