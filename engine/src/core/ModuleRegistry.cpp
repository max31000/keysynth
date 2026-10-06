#include "core/ModuleRegistry.h"

namespace ks {

const char* toString(ModuleKind k) noexcept { return k == ModuleKind::Instrument ? "instrument" : "effect"; }

nlohmann::json toJson(const ModuleInfo& info) {
    nlohmann::json params = nlohmann::json::array();
    for (const auto& p : info.params) params.push_back(toJson(p));
    return {{"typeId", info.typeId},     {"name", info.displayName}, {"kind", toString(info.kind)},
            {"category", info.category}, {"params", params},         {"uiHints", info.uiHints}};
}

void ModuleRegistry::add(const ModuleInfo& info, Factory factory) {
    entries_[info.typeId] = Entry{&info, std::move(factory)};
}

const ModuleInfo* ModuleRegistry::find(const std::string& typeId) const {
    auto it = entries_.find(typeId);
    return it == entries_.end() ? nullptr : it->second.info;
}

std::unique_ptr<Module> ModuleRegistry::create(const std::string& typeId) const {
    auto it = entries_.find(typeId);
    if (it == entries_.end() || !it->second.factory) return nullptr;
    return it->second.factory();
}

std::vector<const ModuleInfo*> ModuleRegistry::list() const {
    std::vector<const ModuleInfo*> out;
    out.reserve(entries_.size());
    for (const auto& [id, e] : entries_) out.push_back(e.info);
    return out;
}

nlohmann::json ModuleRegistry::catalogJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto* info : list()) arr.push_back(toJson(*info));
    return arr;
}

ModuleRegistry& defaultRegistry() {
    static ModuleRegistry* reg = [] {
        auto* r = new ModuleRegistry();
        registerBuiltinModules(*r);
        return r;
    }();
    return *reg;
}

} // namespace ks
