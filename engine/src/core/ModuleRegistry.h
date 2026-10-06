#pragma once
// typeId -> {ModuleInfo, factory}. Built-ins are registered in BuiltinModules.cpp (one line each); plugins add
// `plugin:<id>` entries at runtime (control thread only).

#include "core/Module.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ks {

class ModuleRegistry {
public:
    using Factory = std::function<std::unique_ptr<Module>()>;

    struct Entry {
        const ModuleInfo* info = nullptr;
        Factory factory;
    };

    // Replaces an existing entry with the same typeId (hot reload).
    void add(const ModuleInfo& info, Factory factory);
    bool contains(const std::string& typeId) const { return entries_.count(typeId) != 0; }
    const ModuleInfo* find(const std::string& typeId) const;
    std::unique_ptr<Module> create(const std::string& typeId) const; // nullptr if unknown
    std::vector<const ModuleInfo*> list() const;                     // sorted by typeId
    nlohmann::json catalogJson() const;                             // array of ModuleInfo

    template <typename T>
    void add() {
        add(T::moduleInfo(), [] { return std::make_unique<T>(); });
    }

private:
    std::map<std::string, Entry> entries_;
};

// Registers all built-in modules (BuiltinModules.cpp).
void registerBuiltinModules(ModuleRegistry& registry);

// Process-wide registry with built-ins registered (lazy, control thread).
ModuleRegistry& defaultRegistry();

} // namespace ks
