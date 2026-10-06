#include "platform/DynamicLibrary.h"

#if defined(_WIN32)
#include <windows.h>
#include <malloc.h> // _resetstkoflw
#else
#include <dlfcn.h>
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

bool libraryAvailable(const char* name) noexcept { return ::LoadLibraryA(name) != nullptr; }

namespace {
int filter(unsigned long code, uint32_t* out) noexcept {
    if (out) *out = static_cast<uint32_t>(code);
    // C++ exceptions (0xE06D7363) thrown by a plugin are also caught: they must not cross the C ABI.
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code) noexcept {
    uint32_t c = 0;
    __try {
        fn(ctx);
    } __except (filter(GetExceptionCode(), &c)) {
        if (c == static_cast<uint32_t>(EXCEPTION_STACK_OVERFLOW)) _resetstkoflw();
        if (code) *code = c;
        return false;
    }
    return true;
}

uint32_t currentProcessId() noexcept { return static_cast<uint32_t>(::GetCurrentProcessId()); }

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
bool libraryAvailable(const char* name) noexcept { return ::dlopen(name, RTLD_NOW) != nullptr; }
bool guardedCall(GuardedFn fn, void* ctx, uint32_t*) noexcept {
    fn(ctx);
    return true;
}
uint32_t currentProcessId() noexcept { return static_cast<uint32_t>(::getpid()); }

#endif

} // namespace ks::platform
