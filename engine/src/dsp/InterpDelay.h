#pragma once
// Circular delay line with fractional reads (linear / 4-point Hermite). Buffer allocated in prepare() only;
// push/read are RT-safe. Delay is measured from the most recently pushed sample: read(0) == last pushed,
// read(1) == the one before. A read at fractional delay d interpolates between floor(d) and floor(d)+1.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ks::dsp {

class InterpDelay {
public:
    // Control thread: allocate for delays up to maxDelaySamples (+ interpolation guard).
    void prepare(int maxDelaySamples) {
        size_t n = 4;
        while (n < static_cast<size_t>(maxDelaySamples) + 4) n <<= 1;
        buf_.assign(n, 0.0f);
        mask_ = n - 1;
        w_ = 0;
        maxDelay_ = static_cast<float>(n - 3);
    }
    void reset() noexcept {
        for (float& x : buf_) x = 0.0f;
        w_ = 0;
    }
    float maxDelay() const noexcept { return maxDelay_; }

    // Aliases kept for organ/rotary callers (same semantics).
    void write(float x) noexcept { push(x); }
    float read(float d) const noexcept { return readHermite(d); }

    void push(float x) noexcept {
        w_ = (w_ + 1) & mask_;
        buf_[w_] = x;
    }
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
    }
    // 4-point, 3rd-order Hermite. Needs d >= 1 (one sample of "future" neighbour); clamped.
    float readHermite(float d) const noexcept {
        d = clampDelay(d, 1.0f);
        const float fi = std::floor(d);
        const float t = d - fi;
        const size_t i = static_cast<size_t>(fi);
        const float xm1 = buf_[(w_ - i + 1) & mask_];
        const float x0 = buf_[(w_ - i) & mask_];
        const float x1 = buf_[(w_ - i - 1) & mask_];
        const float x2 = buf_[(w_ - i - 2) & mask_];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

private:
    float clampDelay(float d, float lo) const noexcept {
        if (!(d >= lo)) return lo; // also catches NaN
        return d > maxDelay_ ? maxDelay_ : d;
    }
    std::vector<float> buf_ = std::vector<float>(4, 0.0f);
    size_t mask_ = 3;
    size_t w_ = 0;
    float maxDelay_ = 1.0f;
};

} // namespace ks::dsp
