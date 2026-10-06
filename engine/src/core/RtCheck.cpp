#include "core/RtCheck.h"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace ks::rt {
namespace {
std::atomic<uint64_t> g_violations{0};
std::atomic<uint32_t> g_last{0};
#if defined(KS_RT_CHECKS)
thread_local int t_depth = 0;

bool abortRequested() noexcept {
    static const bool v = [] {
        // getenv does not allocate.
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
        const char* e = std::getenv("KS_RT_ABORT");
        return e != nullptr && e[0] == '1';
    }();
    return v;
}
#endif
} // namespace

#if defined(KS_RT_CHECKS)
bool inRtScope() noexcept { return t_depth > 0; }
void enterScope() noexcept { ++t_depth; }
void leaveScope() noexcept { --t_depth; }
void reportViolation(Violation v) noexcept {
    g_violations.fetch_add(1, std::memory_order_relaxed);
    g_last.store(static_cast<uint32_t>(v), std::memory_order_relaxed);
    if (abortRequested()) std::abort();
}
RtAllowScope::RtAllowScope() noexcept : saved_(t_depth) { t_depth = 0; }
RtAllowScope::~RtAllowScope() { t_depth = saved_; }
#else
RtAllowScope::RtAllowScope() noexcept : saved_(0) {}
RtAllowScope::~RtAllowScope() = default;
#endif

uint64_t violationCount() noexcept { return g_violations.load(std::memory_order_relaxed); }
Violation lastViolation() noexcept { return static_cast<Violation>(g_last.load(std::memory_order_relaxed)); }
void resetViolations() noexcept {
    g_violations.store(0);
    g_last.store(0);
}

} // namespace ks::rt

// -------------------------------------------------------------------------------------------------------
// Global operator new/delete replacement (only with KS_RT_CHECKS). Kept in this TU, which is always linked
// because Engine references ks::rt::enterScope.
#if defined(KS_RT_CHECKS)
namespace {
inline void checkAlloc() noexcept {
    if (ks::rt::t_depth > 0) ks::rt::reportViolation(ks::rt::Violation::Allocation);
}
inline void checkFree(void* p) noexcept {
    if (p != nullptr && ks::rt::t_depth > 0) ks::rt::reportViolation(ks::rt::Violation::Deallocation);
}
void* allocOrThrow(std::size_t n) {
    checkAlloc();
    if (n == 0) n = 1;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* allocAlignedOrThrow(std::size_t n, std::align_val_t a) {
    checkAlloc();
    if (n == 0) n = 1;
#if defined(_MSC_VER)
    if (void* p = _aligned_malloc(n, static_cast<std::size_t>(a))) return p;
#else
    if (void* p = std::aligned_alloc(static_cast<std::size_t>(a), (n + static_cast<std::size_t>(a) - 1) &
                                                                     ~(static_cast<std::size_t>(a) - 1)))
        return p;
#endif
    throw std::bad_alloc();
}
void freeAligned(void* p) noexcept {
    checkFree(p);
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void freePlain(void* p) noexcept {
    checkFree(p);
    std::free(p);
}
} // namespace

void* operator new(std::size_t n) { return allocOrThrow(n); }
void* operator new[](std::size_t n) { return allocOrThrow(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocOrThrow(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocOrThrow(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a) { return allocAlignedOrThrow(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocAlignedOrThrow(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return allocAlignedOrThrow(n, a); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return allocAlignedOrThrow(n, a); } catch (...) { return nullptr; }
}

void operator delete(void* p) noexcept { freePlain(p); }
void operator delete[](void* p) noexcept { freePlain(p); }
void operator delete(void* p, std::size_t) noexcept { freePlain(p); }
void operator delete[](void* p, std::size_t) noexcept { freePlain(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { freePlain(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { freePlain(p); }
void operator delete(void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(p); }
#endif
