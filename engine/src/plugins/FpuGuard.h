#pragma once
// Saves the FP control register (x86 MXCSR / ARM64 FPCR) before a plugin call and restores it afterwards, so a
// plugin that changes FTZ/DAZ, rounding or exception traps cannot leak that into the engine (ARCHITECTURE §4.5:
// FTZ/DAZ re-asserted after plugin calls).

#include "dsp/Denormals.h"

#include <cstdint>

namespace ks::plugins {

class FpuGuard {
public:
    FpuGuard() noexcept {
#if defined(KS_HAS_SSE_CSR)
        saved_ = _mm_getcsr();
#elif defined(KS_HAS_ARM_FPCR)
        saved_ = dsp::readFpcr();
#endif
    }
    ~FpuGuard() { restore(); }
    void restore() const noexcept {
#if defined(KS_HAS_SSE_CSR)
        _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(KS_HAS_ARM_FPCR)
        dsp::writeFpcr(saved_);
#endif
    }
    FpuGuard(const FpuGuard&) = delete;
    FpuGuard& operator=(const FpuGuard&) = delete;

private:
    [[maybe_unused]] uint64_t saved_ = 0;
};

} // namespace ks::plugins
