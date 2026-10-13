#include "platform/DynamicLibrary.h"

#if defined(_WIN32)
#include <windows.h>
#include <malloc.h> // _resetstkoflw

#include <iterator>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <iterator>
#if defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
#endif

namespace ks::platform {

#if defined(_WIN32)

void* openLibrary(const std::filesystem::path& path, std::string& error) {
    // LOAD_WITH_ALTERED_SEARCH_PATH: dependencies next to the DLL are found first.
    HMODULE h = ::LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) error = "LoadLibrary failed (error " + std::to_string(::GetLastError()) + ")";
    return reinterpret_cast<void*>(h);
}

void* findSymbol(void* lib, const char* name) noexcept {
    return lib ? reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(lib), name)) : nullptr;
}

void closeLibrary(void* lib) noexcept {
    if (lib) ::FreeLibrary(reinterpret_cast<HMODULE>(lib));
}


namespace {
int filter(unsigned long code, uint32_t* out) noexcept {
    if (code == EXCEPTION_BREAKPOINT || code == 0x4000001Fu /* WOW64 breakpoint */) return EXCEPTION_CONTINUE_SEARCH;
    if (out) *out = static_cast<uint32_t>(code);
    // C++ exceptions (0xE06D7363) thrown by a plugin are also caught: they must not cross the C ABI.
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code) noexcept {
    uint32_t c = 0;
    bool ok = true;
    __try {
        fn(ctx);
    } __except (filter(GetExceptionCode(), &c)) {
        ok = false;
    }
    if (!ok) {
        // Restore the stack guard page outside the handler (MSDN: _resetstkoflw may fail inside __except).
        if (c == static_cast<uint32_t>(EXCEPTION_STACK_OVERFLOW)) _resetstkoflw();
        if (code) *code = c;
    }
    return ok;
}

uint32_t currentProcessId() noexcept { return static_cast<uint32_t>(::GetCurrentProcessId()); }

bool processAlive(uint32_t pid) noexcept {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return ::GetLastError() == ERROR_ACCESS_DENIED; // exists but not ours to open
    DWORD exitCode = 0;
    const bool alive = ::GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
}

std::filesystem::path executableDir() {
    wchar_t buf[32768];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

#else

void* openLibrary(const std::filesystem::path& path, std::string& error) {
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) error = ::dlerror();
    return h;
}
void* findSymbol(void* lib, const char* name) noexcept { return lib ? ::dlsym(lib, name) : nullptr; }
void closeLibrary(void* lib) noexcept {
    if (lib) ::dlclose(lib);
}

namespace {

constexpr int kFaultSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};

// Per-thread guard state, trivially constructible (no constructor runs; note that on Darwin the first access to a
// thread's TLV block may still malloc once inside dyld — once per thread, outside RtCheck's operator-new hook).
thread_local sigjmp_buf* t_jump = nullptr;
thread_local int t_signal = 0;
thread_local bool t_altStack = false;

struct sigaction g_previous[std::size(kFaultSignals)];
std::atomic<bool> g_installing{false};
// A fault outside any guard was handed to the default action: never reinstall (the re-executed instruction must
// terminate the process, not loop into our handler again).
std::atomic<bool> g_crashing{false};

// Alternate signal stacks (a stack overflow can only be handled on another stack). Static pool of slots, claimed
// lock- and allocation-free and given back by a pthread key destructor when the thread exits (device restarts
// create new audio threads); threads beyond the pool still get the guard, just not for stack overflows.
constexpr size_t kAltStackSize = 64 * 1024;
constexpr int kAltStacks = 16;
alignas(16) unsigned char g_altStackMem[kAltStacks][kAltStackSize];
std::atomic<bool> g_altStackUsed[kAltStacks];
pthread_key_t g_altStackKey;
std::atomic<bool> g_altStackKeyReady{false};

void releaseAltStack(void* value) { // thread exit (pthread key destructor)
    const auto slot = reinterpret_cast<intptr_t>(value) - 1;
    if (slot < 0 || slot >= kAltStacks) return;
    stack_t off{};
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, nullptr);
    g_altStackUsed[slot].store(false, std::memory_order_release);
}

// Windows exception codes for the caught signals (DynamicLibrary.h).
uint32_t windowsCode(int sig) noexcept {
    switch (sig) {
    case SIGSEGV:
    case SIGBUS: return 0xC0000005u; // EXCEPTION_ACCESS_VIOLATION
    case SIGILL: return 0xC000001Du; // EXCEPTION_ILLEGAL_INSTRUCTION
    case SIGFPE: return 0xC0000094u; // EXCEPTION_INT_DIVIDE_BY_ZERO
    default: return 0xC0000001u;
    }
}
constexpr uint32_t kCppExceptionCode = 0xE06D7363u; // MSVC's C++ exception code

void onFault(int sig, siginfo_t* info, void* uctx) {
    if (t_jump != nullptr) {
        t_signal = sig;
        siglongjmp(*t_jump, 1);
    }
    // Not inside guardedCall: hand over to whatever was installed before (crash reporter, default action).
    for (size_t i = 0; i < std::size(kFaultSignals); ++i) {
        if (kFaultSignals[i] != sig) continue;
        const struct sigaction& prev = g_previous[i];
        if (prev.sa_flags & SA_SIGINFO) {
            if (prev.sa_sigaction != nullptr) {
                prev.sa_sigaction(sig, info, uctx);
                return;
            }
        } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
            prev.sa_handler(sig);
            return;
        }
        // Default action: restore it; returning re-executes the faulting instruction, which now terminates.
        g_crashing.store(true);
        signal(sig, SIG_DFL);
        return;
    }
}

bool handlersInstalled() noexcept {
    struct sigaction cur{};
    return sigaction(SIGSEGV, nullptr, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == &onFault;
}

// (Re)install the handlers when someone replaced them (a crash reporter, a test framework that installs its own
// around each test): the replaced handler becomes the one a fault outside guardedCall is forwarded to.
void installHandlers() noexcept {
    if (g_crashing.load()) return;
    if (g_installing.exchange(true)) {
        while (g_installing.load()) {
        }
        return;
    }
    struct sigaction sa{};
    sa.sa_sigaction = &onFault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (!g_altStackKeyReady.load()) // first install only (no allocation on Darwin: fixed TSD slots)
        g_altStackKeyReady.store(pthread_key_create(&g_altStackKey, &releaseAltStack) == 0);
    for (size_t i = 0; i < std::size(kFaultSignals); ++i) {
        struct sigaction prev{};
        if (sigaction(kFaultSignals[i], &sa, &prev) == 0 &&
            !((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction == &onFault))
            g_previous[i] = prev;
    }
    g_installing.store(false);
}

void claimAltStack() noexcept {
    t_altStack = true; // one attempt per thread
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) return; // the thread has one
    if (!g_altStackKeyReady.load()) return; // could not register the release: don't leak a slot
    for (int slot = 0; slot < kAltStacks; ++slot) {
        bool expected = false;
        if (!g_altStackUsed[slot].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
        stack_t st{};
        st.ss_sp = g_altStackMem[slot];
        st.ss_size = kAltStackSize;
        if (sigaltstack(&st, nullptr) != 0) {
            g_altStackUsed[slot].store(false, std::memory_order_release);
            return;
        }
        pthread_setspecific(g_altStackKey, reinterpret_cast<void*>(static_cast<intptr_t>(slot) + 1));
        return;
    }
}

} // namespace

bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code) noexcept {
    // One sigaction query per call (plus sigprocmask in sigsetjmp): a few hundred ns, bounded; it keeps the guard
    // working after another component (crash reporter, test framework) replaced the handlers.
    if (!handlersInstalled()) installHandlers();
    if (!t_altStack) claimAltStack();
    sigjmp_buf jb;
    sigjmp_buf* const outer = t_jump; // nested guards (a plugin call inside another): innermost wins
    // savemask = 1 (one sigprocmask per call): siglongjmp then restores the signal mask and, on Darwin, also the
    // "on alternate stack" state, so a later overflow is still delivered on the alternate stack.
    if (sigsetjmp(jb, 1) == 0) {
        t_jump = &jb;
        try {
            fn(ctx);
        } catch (...) { // must not cross the C ABI (SEH catches these on Windows too)
            t_jump = outer;
            if (code) *code = kCppExceptionCode;
            return false;
        }
        t_jump = outer;
        return true;
    }
    t_jump = outer;
    if (code) *code = windowsCode(t_signal);
    return false;
}

uint32_t currentProcessId() noexcept { return static_cast<uint32_t>(::getpid()); }
bool processAlive(uint32_t pid) noexcept { return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM; }
std::filesystem::path executableDir() {
    std::error_code ec;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    return std::filesystem::weakly_canonical(std::filesystem::path(buf), ec).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

#endif

} // namespace ks::platform
