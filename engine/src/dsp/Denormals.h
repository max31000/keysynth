#pragma once
// RAII flush-to-zero / denormals-are-zero for the current thread (x86 SSE MXCSR, ARM64 FPCR.FZ). No-op elsewhere.
// Equivalent to juce::ScopedNoDenormals, kept here so core/ does not depend on JUCE.

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define KS_HAS_SSE_CSR 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define KS_HAS_ARM_FPCR 1
#endif

namespace ks::dsp {

#if defined(KS_HAS_ARM_FPCR)
// FPCR bits: FZ (24) flushes single/double denormals, FZ16 (19) half precision.
inline constexpr uint64_t kFpcrFlushToZero = (1ull << 24) | (1ull << 19);
inline uint64_t readFpcr() noexcept {
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}
inline void writeFpcr(uint64_t v) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(v)); }
#endif

class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if defined(KS_HAS_SSE_CSR)
        saved_ = _mm_getcsr();
        _mm_setcsr(static_cast<unsigned int>(saved_) | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#elif defined(KS_HAS_ARM_FPCR)
        saved_ = readFpcr();
        writeFpcr(saved_ | kFpcrFlushToZero);
#endif
    }
    ~ScopedNoDenormals() {
#if defined(KS_HAS_SSE_CSR)
        _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(KS_HAS_ARM_FPCR)
        writeFpcr(saved_);
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    [[maybe_unused]] uint64_t saved_ = 0;
};

} // namespace ks::dsp
