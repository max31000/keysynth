#pragma once
// Bucket-brigade delay character model (MN3009/MN3007-style lines in Juno/Solina/flangers), RT-safe.
// Not a clocked-bucket simulation: a modulated fractional delay with the parts that give the sound —
//   - anti-alias / reconstruction low-passes (2-pole Butterworth before and after the line, cutoff ~ BBD clock/3),
//   - mild odd + even soft nonlinearity of the charge transfer,
//   - compander hiss: noise whose level tracks the signal envelope (an NE570 compander makes BBD noise "breathe"
//     with the signal and vanish in silence, so tails still decay to zero).

#include "dsp/InterpDelay.h"
#include "dsp/Math.h"
#include "dsp/ModLfo.h"
#include "dsp/TptFilters.h"

#include <cmath>
#include <cstdint>

namespace ks::dsp {

class Bbd {
public:
    // Settling time of the line's filters + 5 Hz DC blocker after the last delayed sample (for tailSamples()).
    static constexpr float kFilterTailMs = 250.0f;

    // maxDelayMs: longest modulated delay used. cutoffHz: anti-alias / reconstruction corner.
    void prepare(double sampleRate, float maxDelayMs, float cutoffHz) {
        sr_ = static_cast<float>(sampleRate);
        line_.prepare(static_cast<int>(std::ceil(maxDelayMs * 0.001f * sr_)) + 4);
        setCutoff(cutoffHz);
        envCoef_ = onePoleCoef(0.02f, sampleRate);
        dc_.prepare(sampleRate, 5.0f);
        reset();
    }
    void setCutoff(float hz) noexcept {
        // Butterworth 4th-order split into pre (Q .54) and post (Q 1.31) sections, like the input/output filters.
        pre_.set(TptSvf::Type::LowPass, hz, 0.5412f, 0.0f, sr_);
        post_.set(TptSvf::Type::LowPass, hz, 1.3066f, 0.0f, sr_);
    }
    void setHiss(float amount) noexcept { hiss_ = amount * 0.004f; } // 0..1 -> up to ~ -48 dB rel. signal
    void setNoiseSeed(uint32_t s) noexcept { noise_.seed(s); }
    void reset() noexcept {
        line_.reset();
        pre_.reset();
        post_.reset();
        dc_.reset();
        env_ = 0.0f;
    }
    float sampleRate() const noexcept { return sr_; }

    // Push one input sample and read the line at `delaySamples` (>= 1).
    float process(float x, float delaySamples) noexcept {
        float v = pre_.process(x);
        v = std::fmin(std::fmax(v, -4.0f), 4.0f); // keeps the polynomial monotonic
        // Charge-transfer nonlinearity: slight 2nd harmonic + gentle compression of peaks.
        v = v + 0.02f * v * v - 0.02f * v * v * v;
        v = v * (1.0f / (1.0f + 0.04f * std::fabs(v)));
        line_.push(v);
        float y = line_.readHermite(delaySamples);
        const float a = std::fabs(x);
        env_ += (a - env_) * envCoef_;
        y += noise_.next() * hiss_ * env_;
        return dc_.process(post_.process(y));
    }

private:
    float sr_ = 48000.0f;
    InterpDelay line_;
    TptSvf pre_, post_;
    DcBlocker dc_; // the even-order term makes a little DC
    FastNoise noise_;
    float hiss_ = 0.0f;
    float env_ = 0.0f;
    float envCoef_ = 0.01f;
};

} // namespace ks::dsp
