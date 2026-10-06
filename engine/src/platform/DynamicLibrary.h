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
// True if a library with this name can be loaded through the normal search path (e.g. "faust.dll").
// The library stays loaded on success (cheap; used for delay-loaded dependencies).
bool libraryAvailable(const char* name) noexcept;

// Runs fn(ctx) inside a structured-exception guard (SEH __try on Windows; plain call elsewhere).
// Returns false if a hardware exception (access violation, illegal instruction, stack overflow, ...) escaped
// fn; `code` receives the exception code. RT-safe: no allocation. fn must not rely on C++ unwinding.
using GuardedFn = void (*)(void* ctx);
bool guardedCall(GuardedFn fn, void* ctx, uint32_t* code = nullptr) noexcept;

uint32_t currentProcessId() noexcept;

} // namespace ks::platform
