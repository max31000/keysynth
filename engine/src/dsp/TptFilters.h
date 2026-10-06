#pragma once
// Topology-preserving-transform (trapezoidal) filters: stable under fast/abrupt coefficient changes, so they can
// be modulated per sample or per sub-block.
//   TptOnePole: 6 dB/oct low-pass / high-pass / first-order all-pass from one state.
//   TptSvf:     Simper "linear trapezoidal" SVF with arbitrary Q and RBJ-equivalent mixing for
//               LP/HP/BP/notch/all-pass/bell/low-shelf/high-shelf (A. Simper, "SvfLinearTrapOptimised2").

#include "dsp/Math.h"

#include <cmath>

namespace ks::dsp {

// tan(pi f / fs) prewarp with f clamped to a safe range.
inline float tptG(float hz, float sampleRate) noexcept {
    const float f = std::fmin(std::fmax(hz, 1.0f), sampleRate * 0.49f);
    return std::tan(kPi * f / sampleRate);
}

class TptOnePole {
public:
    void setG(float g) noexcept { G_ = g / (1.0f + g); }
    void setCutoff(float hz, float sampleRate) noexcept { setG(tptG(hz, sampleRate)); }
    void reset(float v = 0.0f) noexcept { s_ = v; }
    float lp(float x) noexcept {
        const float v = (x - s_) * G_;
        const float y = v + s_;
        s_ = y + v;
        return y;
    }
    float hp(float x) noexcept { return x - lp(x); }
    // First-order all-pass: 0 deg at DC, -180 deg at Nyquist, -90 deg at the cutoff.
    float ap(float x) noexcept {
        const float l = lp(x);
        return 2.0f * l - x;
    }
    float state() const noexcept { return s_; }

private:
    float G_ = 0.5f;
    float s_ = 0.0f;
};

class TptSvf {
public:
    enum class Type { LowPass, HighPass, BandPass, Notch, AllPass, Bell, LowShelf, HighShelf };

    // gainDb only used by Bell / LowShelf / HighShelf. q > 0.
    void set(Type type, float hz, float q, float gainDb, float sampleRate) noexcept {
        q = std::fmax(q, 0.025f);
        float g = tptG(hz, sampleRate);
        float k = 1.0f / q;
        const float A = std::pow(10.0f, gainDb / 40.0f);
        switch (type) {
        case Type::LowPass: m0_ = 0.0f; m1_ = 0.0f; m2_ = 1.0f; break;
        case Type::HighPass: m0_ = 1.0f; m1_ = -k; m2_ = -1.0f; break;
        case Type::BandPass: m0_ = 0.0f; m1_ = 1.0f; m2_ = 0.0f; break;
        case Type::Notch: m0_ = 1.0f; m1_ = -k; m2_ = 0.0f; break;
        case Type::AllPass: m0_ = 1.0f; m1_ = -2.0f * k; m2_ = 0.0f; break;
        case Type::Bell:
            k = 1.0f / (q * A);
            m0_ = 1.0f; m1_ = k * (A * A - 1.0f); m2_ = 0.0f;
            break;
        case Type::LowShelf:
            g /= std::sqrt(A);
            m0_ = 1.0f; m1_ = k * (A - 1.0f); m2_ = A * A - 1.0f;
            break;
        case Type::HighShelf:
            g *= std::sqrt(A);
            m0_ = A * A; m1_ = k * (1.0f - A) * A; m2_ = 1.0f - A * A;
            break;
        }
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }
    void reset() noexcept { ic1_ = ic2_ = 0.0f; }
    float process(float v0) noexcept {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return m0_ * v0 + m1_ * v1 + m2_ * v2;
    }

private:
    float a1_ = 1.0f, a2_ = 0.0f, a3_ = 0.0f;
    float m0_ = 1.0f, m1_ = 0.0f, m2_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

// One-pole DC blocker (~fc Hz high-pass).
class DcBlocker {
public:
    void prepare(double sampleRate, float hz = 10.0f) noexcept {
        r_ = 1.0f - kTwoPi * hz / static_cast<float>(sampleRate);
    }
    void reset() noexcept { x1_ = y1_ = 0.0f; }
    float process(float x) noexcept {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_ = 0.999f, x1_ = 0.0f, y1_ = 0.0f;
};

} // namespace ks::dsp
