#include "platform/DynamicLibrary.h"

#if defined(_WIN32)
#include <windows.h>
#include <malloc.h> // _resetstkoflw

#include <iterator>
#else
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
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
bool guardedCall(GuardedFn fn, void* ctx, uint32_t*) noexcept {
    fn(ctx);
    return true;
}
uint32_t currentProcessId() noexcept { return static_cast<uint32_t>(::getpid()); }
bool processAlive(uint32_t pid) noexcept { return ::kill(static_cast<pid_t>(pid), 0) == 0; }
std::filesystem::path executableDir() {
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
}

#endif

} // namespace ks::platform
