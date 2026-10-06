#pragma once
// Single-cycle wavetable addressed by a 32-bit phase accumulator (full cycle = 2^32, wraps for free).
// 2048 points + guard, linear interpolation (error < -100 dB for smooth shapes). Built on the control thread
// from a few sine partials; lookup is RT-safe. Only for band-limited content (partials well below Nyquist).

#include "dsp/Math.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace ks::dsp {

class PhaseTable {
public:
    static constexpr int kBits = 11;
    static constexpr int kSize = 1 << kBits;

    // partials: amplitude of harmonic 1, 2, 3, ... (sine phase). Peak normalised to `peak` if > 0.
    void build(std::initializer_list<float> partials, float peak = 0.0f) {
        float mx = 0.0f;
        for (int i = 0; i <= kSize; ++i) {
            const double ph = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / kSize;
            double v = 0.0;
            int h = 1;
            for (float a : partials) v += a * std::sin(ph * h++);
            t_[static_cast<size_t>(i)] = static_cast<float>(v);
            mx = std::fmax(mx, std::fabs(static_cast<float>(v)));
        }
        if (peak > 0.0f && mx > 0.0f)
            for (auto& x : t_) x *= peak / mx;
    }

    float lookup(uint32_t phase) const noexcept {
        const uint32_t i = phase >> (32 - kBits);
        const float f = static_cast<float>(phase & ((1u << (32 - kBits)) - 1u)) * (1.0f / (1u << (32 - kBits)));
        const float a = t_[i];
        return a + (t_[i + 1] - a) * f;
    }

    // Phase increment for a frequency (clamped to < Nyquist).
    static uint32_t increment(double hz, double sampleRate) noexcept {
        double r = hz / sampleRate;
        if (r < 0.0) r = 0.0;
        if (r > 0.499) r = 0.499;
        return static_cast<uint32_t>(r * 4294967296.0);
    }

private:
    std::array<float, kSize + 1> t_{};
};

} // namespace ks::dsp
