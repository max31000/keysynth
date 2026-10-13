#pragma once
// OS specifics for plugin hosting (ARCHITECTURE §10): dynamic libraries and structured-exception guards.
// Only this directory may call Win32.

#include <cstdint>
#include <filesystem>
#include <string>

namespace ks::platform {

// Shared library file extension of this platform (plugin builds, faust runtime): ".dll", ".dylib", ".so".
#if defined(_WIN32)
inline constexpr const char* kSharedLibraryExt = ".dll";
#elif defined(__APPLE__)
inline constexpr const char* kSharedLibraryExt = ".dylib";
#else
inline constexpr const char* kSharedLibraryExt = ".so";
#endif

// LoadLibrary / dlopen. Returns null and sets `error` on failure. Control thread.
void* openLibrary(const std::filesystem::path& path, std::string& error);
void* findSymbol(void* lib, const char* name) noexcept;
void closeLibrary(void* lib) noexcept;

// Runs fn(ctx) inside a hardware-fault guard. Returns false if a fault (access violation, illegal instruction,
// stack overflow, a C++ exception, ...) escaped fn; `code` receives a Windows exception code on every platform, so
// fault messages are identical (POSIX maps SIGSEGV/SIGBUS -> C0000005, SIGILL -> C000001D, SIGFPE -> C0000094,
// a C++ exception -> E06D7363). Breakpoints (__debugbreak / __builtin_trap, asserts in
// Debug plugins) are not caught, so a debugger still stops there. RT-safe: no allocation (POSIX: the signal
// handlers are (re)installed by the call when missing; each thread claims an alternate signal stack from a static pool
// so a stack overflow is caught too). fn must not rely on C++ unwinding: frames between the fault and the guard
// are abandoned (Windows SEH and POSIX siglongjmp alike).
using GuardedFn = void (*)(void* ctx);
bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code = nullptr) noexcept;

uint32_t currentProcessId() noexcept;
// False only if no process with this id exists (used to sweep temp dirs of dead processes).
bool processAlive(uint32_t pid) noexcept;
// Directory of the running executable.
std::filesystem::path executableDir();

} // namespace ks::platform
