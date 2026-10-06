#pragma once
// Circular delay line with fractional (cubic Hermite) read, for modulated delays (Doppler, scanners, chorus).
// Buffer is allocated in prepare() (control thread); write()/read() are RT-safe.
// Convention: write(x) first, then read(d): d = 0 returns the sample just written. Hermite needs one sample
// of history on each side, so reads are clamped to [1, maxDelay].

#include <algorithm>
#include <cmath>
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
    float maxDelay_ = 1.0f;
};

} // namespace ks::dsp
