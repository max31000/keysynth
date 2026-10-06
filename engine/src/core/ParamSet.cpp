#include "core/ParamSet.h"

namespace ks {

ParamSet::ParamSet(const std::vector<ParamSpec>& specs)
    : specs_(&specs), size_(static_cast<int>(specs.size())),
      values_(std::make_unique<std::atomic<float>[]>(specs.size() == 0 ? 1 : specs.size())) {
    resetToDefaults();
}

int ParamSet::indexOf(std::string_view id) const noexcept {
    for (int i = 0; i < size_; ++i)
        if (spec(i).id == id) return i;
    return -1;
}

float ParamSet::set(int index, float value) noexcept {
    if (index < 0 || index >= size_) return 0.0f;
    const float v = spec(index).sanitize(value);
    values_[static_cast<size_t>(index)].store(v, std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
    return v;
}

bool ParamSet::set(std::string_view id, float value) noexcept {
    const int i = indexOf(id);
    if (i < 0) return false;
    set(i, value);
    return true;
}

void ParamSet::resetToDefaults() noexcept {
    for (int i = 0; i < size_; ++i) values_[static_cast<size_t>(i)].store(spec(i).def, std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
}

} // namespace ks
