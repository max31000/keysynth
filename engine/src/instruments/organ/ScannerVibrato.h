#pragma once
// Hammond scanner vibrato / chorus model. The real unit is an LC delay line with taps; a rotating capacitive
// scanner (~6.9 Hz at 60 Hz mains) sweeps across the taps, crossfading between neighbours. V1/V2/V3 sweep a
// growing fraction of the line (deeper pitch modulation); C1/C2/C3 mix the scanned signal with the dry signal.
// Model: delay line, kTaps fixed taps spread over kLineSeconds, scanner position = rounded triangle; output
// crossfades the two adjacent taps (not a pure fractional delay -> the slight tap-ripple of the original),
// then a gentle one-pole low-pass (the line's LC sections roll off the top).

#include "dsp/InterpDelay.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"

#include <algorithm>
#include <cmath>

namespace ks {

class ScannerVibrato {
public:
    enum Mode { Off = 0, V1, V2, V3, C1, C2, C3 };
    static constexpr int kTaps = 9;
    static constexpr float kLineSeconds = 0.0011f; // full-line delay (tap 0 .. tap 8)
    static constexpr float kRateHz = 6.86f;        // scanner speed at 60 Hz
    // Fraction of the line swept per depth (V1/C1, V2/C2, V3/C3).
    static float depthFraction(int depth) noexcept { return depth == 1 ? 0.35f : depth == 2 ? 0.62f : 1.0f; }

    void prepare(double sampleRate) {
        sr_ = static_cast<float>(sampleRate);
        const float line = kLineSeconds * sr_;
        line_.prepare(static_cast<int>(line) + 8);
        for (int i = 0; i < kTaps; ++i) tapDelay_[i] = 1.0f + line * static_cast<float>(i) / (kTaps - 1);
        inc_ = kRateHz / sr_;
        lpCoef_ = 1.0f - std::exp(-dsp::kTwoPi * 7000.0f / sr_);
        dry_.prepare(sampleRate, 0.015f);
        wet_.prepare(sampleRate, 0.015f);
        depth_.prepare(sampleRate, 0.02f);
        setMode(mode_);
        reset();
    }
    void reset() noexcept {
        line_.reset();
        lp_ = 0.0f;
        dry_.snap(dry_.target());
        wet_.snap(wet_.target());
        depth_.snap(depth_.target());
    }
    void setMode(int m) noexcept {
        mode_ = std::clamp(m, 0, 6);
        if (mode_ == Off) {
            dry_.setTarget(1.0f);
            wet_.setTarget(0.0f);
        } else if (mode_ <= V3) {
            dry_.setTarget(0.0f);
            wet_.setTarget(1.0f);
            depth_.setTarget(depthFraction(mode_));
        } else {
            dry_.setTarget(0.6f);
            wet_.setTarget(0.6f);
            depth_.setTarget(depthFraction(mode_ - 3));
        }
    }
    int mode() const noexcept { return mode_; }

    float process(float x) noexcept {
        line_.write(x);
        phase_ += inc_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        const float d = dry_.next();
        const float w = wet_.next();
        const float depth = depth_.next();
        if (w <= 0.0f && !wet_.isSmoothing()) return x * d;
        // Rounded triangle 0..1 (the stator plate geometry smooths the turnarounds).
        const float tri = 1.0f - std::fabs(2.0f * phase_ - 1.0f);
        const float shaped = 0.6f * tri + 0.4f * tri * tri * (3.0f - 2.0f * tri);
        const float pos = shaped * depth * (kTaps - 1);
        const int i = std::min(static_cast<int>(pos), kTaps - 2);
        const float f = pos - static_cast<float>(i);
        const float a = line_.read(tapDelay_[i]);
        const float b = line_.read(tapDelay_[i + 1]);
        lp_ += ((a + (b - a) * f) - lp_) * lpCoef_;
        return x * d + lp_ * w;
    }

private:
    dsp::InterpDelay line_;
    float tapDelay_[kTaps] = {};
    float sr_ = 48000.0f, inc_ = 0.0f, phase_ = 0.0f, lpCoef_ = 0.5f, lp_ = 0.0f;
    int mode_ = Off;
    dsp::OnePoleSmoother dry_, wet_, depth_;
};

} // namespace ks
