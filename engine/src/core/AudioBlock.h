#pragma once
// Non-owning stereo view over two float buffers.

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ks {

struct AudioBlock {
    float* left = nullptr;
    float* right = nullptr;
    int numSamples = 0;

    float* channel(int c) const noexcept { return c == 0 ? left : right; }

    void clear() const noexcept {
        std::memset(left, 0, sizeof(float) * static_cast<size_t>(numSamples));
        std::memset(right, 0, sizeof(float) * static_cast<size_t>(numSamples));
    }

    AudioBlock sub(int offset, int n) const noexcept { return {left + offset, right + offset, n}; }

    void copyFrom(const AudioBlock& o) const noexcept {
        std::memcpy(left, o.left, sizeof(float) * static_cast<size_t>(numSamples));
        std::memcpy(right, o.right, sizeof(float) * static_cast<size_t>(numSamples));
    }

    void addFrom(const AudioBlock& o, float gain = 1.0f) const noexcept {
        for (int i = 0; i < numSamples; ++i) {
            left[i] += o.left[i] * gain;
            right[i] += o.right[i] * gain;
        }
    }

    void applyGain(float g) const noexcept {
        for (int i = 0; i < numSamples; ++i) {
            left[i] *= g;
            right[i] *= g;
        }
    }

    // Peak absolute value per channel.
    void peak(float& l, float& r) const noexcept {
        float pl = 0.0f, pr = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            pl = std::max(pl, std::fabs(left[i]));
            pr = std::max(pr, std::fabs(right[i]));
        }
        l = pl;
        r = pr;
    }
};

} // namespace ks
