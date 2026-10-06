#pragma once
// Circular delay line with fractional (cubic Hermite) read, for modulated delays (Doppler, scanners, chorus).
// Buffer is allocated in prepare() (control thread); write()/read() are RT-safe.
// Convention: write(x) first, then read(d): d = 0 returns the sample just written. Hermite needs one sample
// of history on each side, so reads are clamped to [1, maxDelay].

#include <algorithm>
#include <cmath>
// Circular delay line with fractional reads (linear / 4-point Hermite). Buffer allocated in prepare() only;
// push/read are RT-safe. Delay is measured from the most recently pushed sample: read(0) == last pushed,
// read(1) == the one before. A read at fractional delay d interpolates between floor(d) and floor(d)+1.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ks::dsp {

class InterpDelay {
public:
    // Allocates for delays up to maxDelaySamples (rounded up to a power of two plus interpolation margin).
    void prepare(int maxDelaySamples) {
        int size = 4;
        while (size < maxDelaySamples + 4) size <<= 1;
        buf_.assign(static_cast<size_t>(size), 0.0f);
        mask_ = static_cast<uint32_t>(size - 1);
        maxDelay_ = static_cast<float>(size - 3);
        pos_ = 0;
    }
    void reset() noexcept {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
    }
    float maxDelay() const noexcept { return maxDelay_; }

    void write(float x) noexcept {
        pos_ = (pos_ + 1) & mask_;
        buf_[pos_] = x;
    }

    // Integer-delay tap (no interpolation), 0 = newest.
    float tap(int d) const noexcept { return buf_[(pos_ - static_cast<uint32_t>(d)) & mask_]; }

    // Fractional read, 4-point cubic Hermite. d in samples (clamped to [1, maxDelay]).
    float read(float d) const noexcept {
        d = std::clamp(d, 1.0f, maxDelay_);
        const int di = static_cast<int>(d);
        const float f = d - static_cast<float>(di);
        // Samples around the read point: xm1 is newer (delay di-1), x2 is older (delay di+2).
        const float xm1 = tap(di - 1);
        const float x0 = tap(di);
        const float x1 = tap(di + 1);
        const float x2 = tap(di + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

private:
    std::vector<float> buf_ = std::vector<float>(4, 0.0f);
    uint32_t mask_ = 3;
    uint32_t pos_ = 0;
    // Control thread: allocate for delays up to maxDelaySamples (+ interpolation guard).
        size_t n = 4;
        while (n < static_cast<size_t>(maxDelaySamples) + 4) n <<= 1;
        buf_.assign(n, 0.0f);
        mask_ = n - 1;
        w_ = 0;
        maxDelay_ = static_cast<float>(n - 3);
        for (float& x : buf_) x = 0.0f;
    void push(float x) noexcept {
        w_ = (w_ + 1) & mask_;
        buf_[w_] = x;
    // Integer tap, 0 <= d <= maxDelay.
    float tap(int d) const noexcept { return buf_[(w_ - static_cast<size_t>(d)) & mask_]; }
    float readLinear(float d) const noexcept {
        d = clampDelay(d, 0.0f);
        const float fi = std::floor(d);
        const float t = d - fi;
        const size_t i = static_cast<size_t>(fi);
        const float a = buf_[(w_ - i) & mask_];
        const float b = buf_[(w_ - i - 1) & mask_];
        return a + (b - a) * t;
    // 4-point, 3rd-order Hermite. Needs d >= 1 (one sample of "future" neighbour); clamped.
    float readHermite(float d) const noexcept {
        d = clampDelay(d, 1.0f);
        const float xm1 = buf_[(w_ - i + 1) & mask_];
        const float x0 = buf_[(w_ - i) & mask_];
        const float x1 = buf_[(w_ - i - 1) & mask_];
        const float x2 = buf_[(w_ - i - 2) & mask_];
        return ((c3 * t + c2) * t + c1) * t + x0;
    float clampDelay(float d, float lo) const noexcept {
        if (!(d >= lo)) return lo; // also catches NaN
        return d > maxDelay_ ? maxDelay_ : d;
    size_t mask_ = 3;
    size_t w_ = 0;
    float maxDelay_ = 1.0f;
};

} // namespace ks::dsp
