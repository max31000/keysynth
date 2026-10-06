#pragma once
// Real-time rule enforcement (ARCHITECTURE §4.8).
//
// RtScope marks the current thread as "inside the audio callback". With KS_RT_CHECKS, the replaced global
// operator new/delete (RtCheck.cpp) and ks::Mutex record a violation whenever they are used inside such a
// scope. Violations are counted (never allocate/print from the hook); tests assert the count stays 0. Set the
// environment variable KS_RT_ABORT=1 to abort on the first violation (useful under a debugger).

#include <cstdint>
#include <mutex>

namespace ks::rt {

enum class Violation : uint32_t { None = 0, Allocation = 1, Deallocation = 2, Lock = 3 };

#if defined(KS_RT_CHECKS)
bool inRtScope() noexcept;
void enterScope() noexcept;
void leaveScope() noexcept;
void reportViolation(Violation v) noexcept;
#else
inline bool inRtScope() noexcept { return false; }
inline void enterScope() noexcept {}
inline void leaveScope() noexcept {}
inline void reportViolation(Violation) noexcept {}
#endif

// Total violations since start (or last reset). Always available (0 when checks are compiled out).
uint64_t violationCount() noexcept;
Violation lastViolation() noexcept;
void resetViolations() noexcept;
constexpr bool checksEnabled() noexcept {
#if defined(KS_RT_CHECKS)
    return true;
#else
    return false;
#endif
}

// RAII marker for audio-thread code. Nestable.
class RtScope {
public:
    RtScope() noexcept { enterScope(); }
    ~RtScope() { leaveScope(); }
    RtScope(const RtScope&) = delete;
    RtScope& operator=(const RtScope&) = delete;
};

// Temporarily allow non-RT operations inside an RtScope (only for code that is *known* to be off the hot path,
// e.g. one-time thread registration on the first callback). Use sparingly; reviewers check every use.
class RtAllowScope {
public:
    RtAllowScope() noexcept;
    ~RtAllowScope();
    RtAllowScope(const RtAllowScope&) = delete;
    RtAllowScope& operator=(const RtAllowScope&) = delete;
private:
    int saved_;
};

} // namespace ks::rt

namespace ks {

// Mutex wrapper for control-side code. Locking it inside an RtScope is a violation.
class Mutex {
public:
    void lock() {
        if (rt::inRtScope()) rt::reportViolation(rt::Violation::Lock);
        m_.lock();
    }
    bool try_lock() {
        if (rt::inRtScope()) rt::reportViolation(rt::Violation::Lock);
        return m_.try_lock();
    }
    void unlock() { m_.unlock(); }
private:
    std::mutex m_;
};

using MutexLock = std::lock_guard<Mutex>;

} // namespace ks
