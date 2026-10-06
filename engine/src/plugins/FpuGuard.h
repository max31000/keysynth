#pragma once
// Saves MXCSR before a plugin call and restores it afterwards, so a plugin that changes FTZ/DAZ, rounding or
// exception masks cannot leak that into the engine (ARCHITECTURE §4.5: FTZ/DAZ re-asserted after plugin calls).

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define KS_FPU_GUARD_SSE 1
#endif

namespace ks::plugins {

class FpuGuard {
public:
    FpuGuard() noexcept {
#if defined(KS_FPU_GUARD_SSE)
        saved_ = _mm_getcsr();
#endif
    }
    ~FpuGuard() { restore(); }
    void restore() const noexcept {
#if defined(KS_FPU_GUARD_SSE)
        _mm_setcsr(saved_);
#endif
    }
    FpuGuard(const FpuGuard&) = delete;
    FpuGuard& operator=(const FpuGuard&) = delete;

private:
    unsigned int saved_ = 0;
};

} // namespace ks::plugins
