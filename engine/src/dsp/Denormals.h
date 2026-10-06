#pragma once
// RAII flush-to-zero / denormals-are-zero for the current thread (x86 SSE). No-op elsewhere.
// Equivalent to juce::ScopedNoDenormals, kept here so core/ does not depend on JUCE.

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define KS_HAS_SSE_CSR 1
#endif

namespace ks::dsp {

class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if defined(KS_HAS_SSE_CSR)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
    }
    ~ScopedNoDenormals() {
#if defined(KS_HAS_SSE_CSR)
        _mm_setcsr(saved_);
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    unsigned int saved_ = 0;
};

} // namespace ks::dsp
