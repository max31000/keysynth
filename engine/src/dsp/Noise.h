#pragma once
// Noise / random helpers. RT-safe, deterministic per seed.

#include <cstdint>

namespace ks::dsp {

// 32-bit integer hash (lowbias32). Good avalanche; used for stateless sample&hold values.
inline uint32_t hash32(uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Map a 32-bit integer to a float in [-1, 1).
inline float bipolarFromBits(uint32_t x) noexcept {
    return static_cast<float>(static_cast<int32_t>(x)) * (1.0f / 2147483648.0f);
}

// xorshift32 white noise.
class WhiteNoise {
public:
    WhiteNoise() noexcept = default;
    explicit WhiteNoise(uint32_t seed) noexcept { setSeed(seed); }
    void setSeed(uint32_t seed) noexcept { state_ = seed ? seed : 0x9e3779b9u; }
    uint32_t nextBits() noexcept {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    // Uniform in [-1, 1).
    float next() noexcept { return bipolarFromBits(nextBits()); }
    // Uniform in [0, 1).
    float nextUnipolar() noexcept { return static_cast<float>(nextBits() >> 8) * (1.0f / 16777216.0f); }

private:
    uint32_t state_ = 0x12345678u;
};

} // namespace ks::dsp
