#pragma once
// OS specifics for plugin hosting (ARCHITECTURE §10): dynamic libraries and structured-exception guards.
// Only this directory may call Win32.

#include <cstdint>
#include <filesystem>
#include <string>

namespace ks::platform {

// LoadLibrary / dlopen. Returns null and sets `error` on failure. Control thread.
void* openLibrary(const std::filesystem::path& path, std::string& error);
void* findSymbol(void* lib, const char* name) noexcept;
void closeLibrary(void* lib) noexcept;

// Runs fn(ctx) inside a structured-exception guard (SEH __try on Windows; plain call elsewhere).
// Returns false if a hardware exception (access violation, illegal instruction, stack overflow, ...) escaped
// fn; `code` receives the exception code. Breakpoints (__debugbreak, asserts in Debug plugins) are not caught,
// so a debugger still stops there. RT-safe: no allocation. fn must not rely on C++ unwinding.
using GuardedFn = void (*)(void* ctx);
bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code = nullptr) noexcept;

uint32_t currentProcessId() noexcept;
// False only if no process with this id exists (used to sweep temp dirs of dead processes).
bool processAlive(uint32_t pid) noexcept;
// Directory of the running executable.
std::filesystem::path executableDir();

} // namespace ks::platform
