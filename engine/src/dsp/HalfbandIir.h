#pragma once
// Minimum-phase-ish IIR oversampling (ARCHITECTURE §4.7: no linear-phase FIR, zero reported latency).
// Polyphase half-band filters built from two parallel chains of first-order all-passes (L. de Soras, "HIIR"
// design: elliptic half-band via the Jacobi-theta closed form). Group delay is a few samples at the base rate and
// frequency dependent; no lookahead is introduced.
//
//   designHalfband(coefs, n, transitionBw)  coefficients for n all-pass stages (n even recommended)
//   Upsampler2x / Downsampler2x             one stage (per channel)
//   Oversampler                             1x / 2x / 4x wrapper, per channel, fixed max block

#include <array>
#include <cmath>
#include <vector>

namespace ks::dsp {

// transitionBw: normalized (0..0.5) width of the transition band relative to the *oversampled* rate's half-band
// edge (e.g. 0.04 -> passband to 0.46*fs_base). Control thread (uses double math).
inline void designHalfband(double* coefs, int numCoefs, double transitionBw) {
    const double pi = 3.14159265358979323846;
    double k = std::tan((1.0 - transitionBw * 2.0) * pi / 4.0);
    k *= k;
    const double kksqrt = std::pow(1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e;
    const double e4 = e2 * e2;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    const int order = numCoefs * 2 + 1;
    for (int index = 0; index < numCoefs; ++index) {
        const int c = index + 1;
        double num = 0.0;
        {
            int i = 0, j = 1;
            double term;
            do {
                term = std::pow(q, static_cast<double>(i * (i + 1))) * std::sin((i * 2 + 1) * c * pi / order) * j;
                num += term;
                j = -j;
                ++i;
            } while (std::fabs(term) > 1e-100 && i < 100);
        }
        double den = 0.0;
        {
            int i = 1, j = -1;
            double term;
            do {
                term = std::pow(q, static_cast<double>(i * i)) * std::cos(i * 2 * c * pi / order) * j;
                den += term;
                j = -j;
                ++i;
            } while (std::fabs(term) > 1e-100 && i < 100);
        }
        num *= std::pow(q, 0.25);
        den += 0.5;
        const double ww = num / den;
        const double wwsq = ww * ww;
        const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        coefs[index] = (1.0 - x) / (1.0 + x);
    }
}

constexpr int kHalfbandCoefs = 8; // ~ -90 dB stopband with transition 0.04 (de Soras' table)

// Two all-pass chains (even / odd coefficients) shared by up- and down-samplers.
class HalfbandPair {
public:
    void setCoefs(const double* c) noexcept {
        for (int i = 0; i < kHalfbandCoefs / 2; ++i) {
            a_[static_cast<size_t>(i)] = static_cast<float>(c[2 * i]);
            b_[static_cast<size_t>(i)] = static_cast<float>(c[2 * i + 1]);
        }
    }
    void reset() noexcept {
        xa_.fill(0.0f); ya_.fill(0.0f);
        xb_.fill(0.0f); yb_.fill(0.0f);
    }
    float pathA(float x) noexcept { return chain(x, a_, xa_, ya_); }
    float pathB(float x) noexcept { return chain(x, b_, xb_, yb_); }

private:
    using Arr = std::array<float, kHalfbandCoefs / 2>;
    static float chain(float x, const Arr& c, Arr& mx, Arr& my) noexcept {
        for (size_t i = 0; i < c.size(); ++i) {
            const float y = (x - my[i]) * c[i] + mx[i];
            mx[i] = x;
            my[i] = y;
            x = y;
        }
        return x;
    }
    Arr a_{}, b_{}, xa_{}, ya_{}, xb_{}, yb_{};
};

class Upsampler2x {
public:
    void setCoefs(const double* c) noexcept { f_.setCoefs(c); }
    void reset() noexcept { f_.reset(); }
    // One input sample -> two output samples.
    void process(float x, float& o0, float& o1) noexcept {
        o0 = f_.pathA(x);
        o1 = f_.pathB(x);
    }

private:
    HalfbandPair f_;
};

class Downsampler2x {
public:
    void setCoefs(const double* c) noexcept { f_.setCoefs(c); }
    void reset() noexcept { f_.reset(); }
    // Two input samples (x0 earlier) -> one output sample.
    float process(float x0, float x1) noexcept { return 0.5f * (f_.pathA(x1) + f_.pathB(x0)); }

private:
    HalfbandPair f_;
};

// Per-channel 1x/2x/4x oversampler. Usage per block: n = up(in, n) -> process buffer() of n*factor samples ->
// down(out, n). Buffer sized for maxBlock in prepare (control thread).
class Oversampler {
public:
    static constexpr int kMaxFactor = 4;

    void prepare(int maxBlock) {
        static const std::array<double, kHalfbandCoefs> c1 = [] {
            std::array<double, kHalfbandCoefs> c{};
            designHalfband(c.data(), kHalfbandCoefs, 0.04); // stage 1: passband to ~0.46 fs_base
            return c;
        }();
        static const std::array<double, kHalfbandCoefs> c2 = [] {
            std::array<double, kHalfbandCoefs> c{};
            designHalfband(c.data(), kHalfbandCoefs, 0.25); // stage 2: wide transition is enough (images far)
            return c;
        }();
        up1_.setCoefs(c1.data()); dn1_.setCoefs(c1.data());
        up2_.setCoefs(c2.data()); dn2_.setCoefs(c2.data());
        buf_.assign(static_cast<size_t>(maxBlock) * kMaxFactor, 0.0f);
        reset();
    }
    void reset() noexcept {
        up1_.reset(); dn1_.reset(); up2_.reset(); dn2_.reset();
    }
    void setFactor(int f) noexcept { factor_ = f >= 4 ? 4 : (f >= 2 ? 2 : 1); }
    int factor() const noexcept { return factor_; }
    float* buffer() noexcept { return buf_.data(); }

    void up(const float* in, int n) noexcept {
        float* b = buf_.data();
        if (factor_ == 1) {
            for (int i = 0; i < n; ++i) b[i] = in[i];
        } else if (factor_ == 2) {
            for (int i = 0; i < n; ++i) up1_.process(in[i], b[2 * i], b[2 * i + 1]);
        } else {
            for (int i = 0; i < n; ++i) {
                float a0, a1;
                up1_.process(in[i], a0, a1);
                up2_.process(a0, b[4 * i], b[4 * i + 1]);
                up2_.process(a1, b[4 * i + 2], b[4 * i + 3]);
            }
        }
    }
    void down(float* out, int n) noexcept {
        const float* b = buf_.data();
        if (factor_ == 1) {
            for (int i = 0; i < n; ++i) out[i] = b[i];
        } else if (factor_ == 2) {
            for (int i = 0; i < n; ++i) out[i] = dn1_.process(b[2 * i], b[2 * i + 1]);
        } else {
            for (int i = 0; i < n; ++i) {
                const float a0 = dn2_.process(b[4 * i], b[4 * i + 1]);
                const float a1 = dn2_.process(b[4 * i + 2], b[4 * i + 3]);
                out[i] = dn1_.process(a0, a1);
            }
        }
    }

private:
    Upsampler2x up1_, up2_;
    Downsampler2x dn1_, dn2_;
    std::vector<float> buf_;
    int factor_ = 2;
};

} // namespace ks::dsp
