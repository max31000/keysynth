#pragma once
// Band-limited oscillators with exact sub-sample discontinuity placement:
//   BlepOsc  - saw / pulse (DC-free PWM) / triangle (PolyBLAMP corners) / sine, hard sync input, wrap output.
//   SuperSaw - 7 detuned saws (Roland JP-8000 detune/mix curves after A. Szabo, "How to Emulate the Super
//              Saw", 2010), PolyBLEP per saw.
// Both use the 2-sample PolyBLEP/PolyBLAMP residuals, applied on both sides of each discontinuity. That needs
// one sample of look-ahead, so the output is the waveform delayed by one sample (21 us at 48 kHz). Oscillators
// mixed together are delayed equally, so sync/ring/FM relations are preserved. RT-safe, no allocation.

#include "dsp/Math.h"

#include <cmath>

namespace ks::dsp {

// Residuals for a discontinuity that happened `d` samples (0 <= d < 1) before the current sample.
// pre* goes to the previous output sample, post* to the current one. Step residuals are per unit height,
// ramp residuals per unit change of slope (amplitude per sample).
inline float blepPre(float d) noexcept { return 0.5f * d * d; }
inline float blepPost(float d) noexcept {
    const float a = 1.0f - d;
    return -0.5f * a * a;
}
inline float blampPre(float d) noexcept { return d * d * d * (1.0f / 6.0f); }
inline float blampPost(float d) noexcept {
    const float a = 1.0f - d;
    return a * a * a * (1.0f / 6.0f);
}

enum class OscWave { Saw = 0, Pulse = 1, Triangle = 2, Sine = 3 };

class BlepOsc {
public:
    void reset(float phase = 0.0f) noexcept {
        phase_ = phase - std::floor(phase);
        held_ = 0.0f;
        wrapD_ = -1.0f;
    }
    float phase() const noexcept { return phase_; }
    // Time since this oscillator's natural wrap during the last tick (samples, [0,1)), or -1 if none.
    // Feed it as `syncD` to a slave oscillator ticked in the same sample.
    float wrapD() const noexcept { return wrapD_; }

    // Naive waveform value at phase p (pulse is DC-compensated so PWM does not move the DC level).
    static float naive(OscWave w, float p, float pw) noexcept {
        switch (w) {
        case OscWave::Saw: return 2.0f * p - 1.0f;
        case OscWave::Pulse: return (p < pw ? 1.0f : -1.0f) - (2.0f * pw - 1.0f);
        case OscWave::Triangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        case OscWave::Sine: return std::sin(kTwoPi * p);
        }
        return 0.0f;
    }

    // Advance one sample. inc = cycles/sample (clamped to [0, 0.45]); pw in (0,1) for Pulse.
    // syncD >= 0: hard-sync reset happened syncD samples before this sample.
    float tick(OscWave w, float inc, float pw = 0.5f, float syncD = -1.0f) noexcept {
        inc = std::fmin(std::fmax(inc, 0.0f), 0.45f);
        pw = std::fmin(std::fmax(pw, 0.01f), 0.99f);
        wrapD_ = -1.0f;
        float pre = 0.0f, cur = 0.0f;
        float p = phase_;
        if (syncD >= 0.0f && syncD < 1.0f && inc > 1e-9f) {
            float pe = p + inc * (1.0f - syncD); // slave phase at the sync instant
            events(w, p, pe, syncD, inc, pw, pre, cur);
            if (pe >= 1.0f) pe -= 1.0f;
            const float h = naive(w, 0.0f, pw) - naive(w, pe, pw);
            pre += h * blepPre(syncD);
            cur += h * blepPost(syncD);
            if (w == OscWave::Triangle) {
                const float ds = 4.0f * inc - (pe < 0.5f ? 4.0f * inc : -4.0f * inc);
                pre += ds * blampPre(syncD);
                cur += ds * blampPost(syncD);
            }
            p = inc * syncD;
            events(w, 0.0f, p, 0.0f, inc, pw, pre, cur);
        } else {
            float pe = p + inc;
            events(w, p, pe, 0.0f, inc, pw, pre, cur);
            if (pe >= 1.0f) {
                wrapD_ = (pe - 1.0f) / inc;
                pe -= 1.0f;
                if (pe >= 1.0f) pe = 0.0f; // numeric guard
            }
            p = pe;
        }
        phase_ = p;
        const float out = held_ + pre;
        held_ = naive(w, p, pw) + cur;
        return out;
    }

private:
    // Accumulate residuals of every natural discontinuity in phase interval (a, b]; the interval ends dEnd
    // samples before the current sample.
    static void events(OscWave w, float a, float b, float dEnd, float inc, float pw, float& pre,
                       float& cur) noexcept {
        if (b <= a || inc <= 1e-9f) return;
        // Divisions only on the (rare) samples that contain a discontinuity.
        auto step = [&](float e, float h) {
            if (e > a && e <= b) {
                const float d = dEnd + (b - e) / inc;
                pre += h * blepPre(d);
                cur += h * blepPost(d);
            }
        };
        auto ramp = [&](float e, float ds) {
            if (e > a && e <= b) {
                const float d = dEnd + (b - e) / inc;
                pre += ds * blampPre(d);
                cur += ds * blampPost(d);
            }
        };
        switch (w) {
        case OscWave::Saw: step(1.0f, -2.0f); break;
        case OscWave::Pulse:
            step(pw, -2.0f);
            step(1.0f, 2.0f);
            step(1.0f + pw, -2.0f);
            break;
        case OscWave::Triangle:
            ramp(0.5f, -8.0f * inc);
            ramp(1.0f, 8.0f * inc);
            ramp(1.5f, -8.0f * inc);
            break;
        case OscWave::Sine: break;
        }
    }

    float phase_ = 0.0f;
    float held_ = 0.0f;
    float wrapD_ = -1.0f;
};

class SuperSaw {
public:
    static constexpr int kSaws = 7;

    // Szabo's fit of the JP-8000 detune knob (x in 0..1) -> relative detune scale.
    static float detuneCurve(float x) noexcept {
        x = std::fmin(std::fmax(x, 0.0f), 1.0f);
        constexpr double c[] = {10028.7312891634, -50818.8652045924, 111363.4808729368, -138150.6761080548,
                                106649.6679158292, -53046.9642751875, 17019.9518580080,  -3425.0836591318,
                                404.2703938388,    -24.1878824391,    0.6717417634,      0.0030115596};
        double y = 0.0;
        for (double k : c) y = y * x + k;
        return static_cast<float>(y);
    }

    // detune, mix in 0..1. Call at control rate.
    void setShape(float detune, float mix) noexcept {
        static constexpr float kOffsets[kSaws] = {-0.11002313f, -0.06288439f, -0.01952356f, 0.0f,
                                                  0.01991221f,  0.06216538f,  0.10745242f};
        const float dt = detuneCurve(detune);
        mix = std::fmin(std::fmax(mix, 0.0f), 1.0f);
        const float centre = -0.55366f * mix + 0.99785f;
        const float side = -0.73764f * mix * mix + 1.2841f * mix + 0.044372f;
        const float norm = 1.0f / std::sqrt(centre * centre + 6.0f * side * side); // saws are uncorrelated
        for (int i = 0; i < kSaws; ++i) {
            ratio_[i] = 1.0f + dt * kOffsets[i];
            gain_[i] = (i == 3 ? centre : side) * norm;
        }
    }

    // Phases from a 32-bit seed (JP-8000 saws free-run with unrelated phases).
    void reset(unsigned seed) noexcept {
        unsigned s = seed * 2654435761u + 1u;
        for (float& ph : phase_) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            ph = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
        }
        held_ = 0.0f;
        wrapD_ = -1.0f;
    }
    float wrapD() const noexcept { return wrapD_; } // wrap of the centre saw (sync master)

    // Base increment (cycles/sample of the centre saw). Call at control rate, after setShape().
    void setInc(float inc) noexcept {
        for (int i = 0; i < kSaws; ++i) inc_[i] = std::fmin(std::fmax(inc * ratio_[i], 0.0f), 0.45f);
    }

    // `fmScale` multiplies every saw's increment for this sample (linear FM), >= 0.
    float tick(float fmScale = 1.0f) noexcept {
        float pre = 0.0f, cur = 0.0f, sum = 0.0f;
        wrapD_ = -1.0f;
        for (int i = 0; i < kSaws; ++i) {
            const float di = std::fmin(inc_[i] * fmScale, 0.45f);
            float p = phase_[i] + di;
            if (p >= 1.0f) {
                p -= 1.0f;
                const float d = di > 1e-9f ? p / di : 0.0f;
                pre -= 2.0f * gain_[i] * blepPre(d);
                cur -= 2.0f * gain_[i] * blepPost(d);
                if (i == 3) wrapD_ = d;
            }
            phase_[i] = p;
            sum += gain_[i] * (2.0f * p - 1.0f);
        }
        const float out = held_ + pre;
        held_ = sum + cur;
        return out;
    }

private:
    float phase_[kSaws] = {};
    float ratio_[kSaws] = {1, 1, 1, 1, 1, 1, 1};
    float inc_[kSaws] = {};
    float gain_[kSaws] = {0, 0, 0, 1, 0, 0, 0};
    float held_ = 0.0f;
    float wrapD_ = -1.0f;
};

} // namespace ks::dsp
