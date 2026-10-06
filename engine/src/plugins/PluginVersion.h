#pragma once
// One loaded version of a plugin (ARCHITECTURE §10). Owns the ModuleInfo the registry points at and the
// compiled code (Faust factory or DLL). Shared by the registry entry (factory lambda) and every module created
// from it, so the code outlives all its instances; the last owner releases it on the control thread.

#include "core/Module.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ks::plugins {

enum class PluginSource { Faust, Dll };

// Fault code used when a plugin produced NaN/Inf (the block is zeroed and the instance cleared, not muted).
inline constexpr uint32_t kFaultNonFinite = 0xE0000001u;
// Fault code used when an instance could not be created/prepared (module stays silent).
inline constexpr uint32_t kFaultNoInstance = 0xE0000002u;

class PluginVersion : public std::enable_shared_from_this<PluginVersion> {
public:
    virtual ~PluginVersion() = default;
    // Control thread. nullptr on failure.
    virtual std::unique_ptr<Module> createModule() const = 0;

    std::string name;   // directory name under plugins/
    PluginSource source = PluginSource::Faust;
    int version = 0;    // increments on every successful (re)load
    ModuleInfo info;    // typeId "plugin:<name>"

    // Audio thread -> control thread: set when an instance faulted (SEH) or produced NaN/Inf.
    mutable std::atomic<uint32_t> faults{0};
    mutable std::atomic<uint32_t> faultCode{0};
    void reportFault(uint32_t code) const noexcept {
        faultCode.store(code, std::memory_order_relaxed);
        faults.fetch_add(1, std::memory_order_release);
    }
};

} // namespace ks::plugins
