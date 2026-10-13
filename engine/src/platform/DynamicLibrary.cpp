#include "platform/DynamicLibrary.h"

#if defined(_WIN32)
#include <windows.h>
#include <malloc.h> // _resetstkoflw

#include <iterator>
#else
#include <dlfcn.h>
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

// Per-thread guard state. Plain (trivially constructible) thread_locals: no allocation on first use.
thread_local sigjmp_buf* t_jump = nullptr;
thread_local int t_signal = 0;
thread_local bool t_altStack = false;

struct sigaction g_previous[std::size(kFaultSignals)];
std::atomic<int> g_installed{0}; // 0 none, 1 installing, 2 done

// Alternate signal stacks (a stack overflow can only be handled on another stack). Static pool: claiming one is
// lock- and allocation-free; threads beyond the pool still get the guard, just not for stack overflows.
constexpr size_t kAltStackSize = 64 * 1024;
constexpr int kAltStacks = 16;
alignas(16) unsigned char g_altStackMem[kAltStacks][kAltStackSize];
std::atomic<int> g_altStackNext{0};

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
        signal(sig, SIG_DFL);
        return;
    }
}

void installHandlers() noexcept {
    int expected = 0;
    if (!g_installed.compare_exchange_strong(expected, 1)) {
        while (g_installed.load(std::memory_order_acquire) != 2) {
        }
        return;
    }
    struct sigaction sa{};
    sa.sa_sigaction = &onFault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < std::size(kFaultSignals); ++i) sigaction(kFaultSignals[i], &sa, &g_previous[i]);
    g_installed.store(2, std::memory_order_release);
}

void claimAltStack() noexcept {
    t_altStack = true; // one attempt per thread
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) return; // the thread has one
    const int slot = g_altStackNext.fetch_add(1);
    if (slot >= kAltStacks) return;
    stack_t st{};
    st.ss_sp = g_altStackMem[slot];
    st.ss_size = kAltStackSize;
    sigaltstack(&st, nullptr);
}

} // namespace

bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code) noexcept {
    if (g_installed.load(std::memory_order_acquire) != 2) installHandlers();
    if (!t_altStack) claimAltStack();
    sigjmp_buf jb;
    sigjmp_buf* const outer = t_jump; // nested guards (a plugin call inside another): innermost wins
    // savemask = 1 (one sigprocmask per call): siglongjmp then restores the signal mask and, on Darwin, also the
    // "on alternate stack" state, so a later overflow is still delivered on the alternate stack.
    if (sigsetjmp(jb, 1) == 0) {
        t_jump = &jb;
        fn(ctx);
        t_jump = outer;
        return true;
    }
    t_jump = outer;
    if (code) *code = static_cast<uint32_t>(t_signal);
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
