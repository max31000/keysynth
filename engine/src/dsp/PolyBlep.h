#pragma once
// Band-limited (PolyBLEP) oscillator: saw, square/pulse (PWM), triangle (leaky-integrated square), sine.
// Phase in [0,1). Aliasing is reduced by a 2-sample polynomial residual at each discontinuity.

#include "dsp/Math.h"

namespace ks::dsp {

enum class Wave { Saw = 0, Square = 1, Triangle = 2, Sine = 3 };

class PolyBlepOsc {
public:
    void setSampleRate(double sr) noexcept { sampleRate_ = static_cast<float>(sr); }
    void setFrequency(float hz) noexcept {
        inc_ = hz / sampleRate_;
        if (inc_ > 0.49f) inc_ = 0.49f;
        if (inc_ < 0.0f) inc_ = 0.0f;
    }
    void setPulseWidth(float pw) noexcept { pw_ = std::fmin(std::fmax(pw, 0.02f), 0.98f); }
    void resetPhase(float p = 0.0f) noexcept {
        phase_ = p;
        tri_ = 0.0f;
    }
    float phase() const noexcept { return phase_; }

    float next(Wave w) noexcept {
        float out = 0.0f;
        const float t = phase_;
        switch (w) {
        case Wave::Saw: out = 2.0f * t - 1.0f - blep(t, inc_); break;
        case Wave::Square: out = pulse(t); break;
        case Wave::Triangle: {
            // Integrate the band-limited square; leak keeps DC bounded.
            const float sq = pulse(t);
            tri_ = tri_ * 0.999f + sq * inc_ * 4.0f;
            out = tri_;
            break;
        }
        case Wave::Sine: out = std::sin(kTwoPi * t); break;
        }
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        return out;
    }

    // Polynomial BLEP residual for a unit step at phase 0 (t = phase, dt = increment).
    static float blep(float t, float dt) noexcept {
        if (dt <= 0.0f) return 0.0f;
        if (t < dt) {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

private:
    float pulse(float t) const noexcept {
        float v = t < pw_ ? 1.0f : -1.0f;
        v += blep(t, inc_);
        float t2 = t - pw_;
        if (t2 < 0.0f) t2 += 1.0f;
        v -= blep(t2, inc_);
        return v;
    }

    float sampleRate_ = 48000.0f;
    float inc_ = 0.0f;
    float phase_ = 0.0f;
    float pw_ = 0.5f;
    float tri_ = 0.0f;
};

} // namespace ks::dsp
