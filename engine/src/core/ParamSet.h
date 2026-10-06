#pragma once
// Fixed array of atomic floats (plain units), one per ParamSpec (ARCHITECTURE §5.1).
// Control thread writes (set), audio thread reads once per block (get). Allocated once at construction.

#include "core/ParamSpec.h"

#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

namespace ks {

class ParamSet {
public:
    explicit ParamSet(const std::vector<ParamSpec>& specs);
    ParamSet(const ParamSet&) = delete;
    ParamSet& operator=(const ParamSet&) = delete;

    int size() const noexcept { return size_; }
    const ParamSpec& spec(int index) const noexcept { return (*specs_)[static_cast<size_t>(index)]; }
    const std::vector<ParamSpec>& specs() const noexcept { return *specs_; }

    // -1 if unknown. Linear scan (control thread only; param counts are small).
    int indexOf(std::string_view id) const noexcept;

    // RT-safe read.
    float get(int index) const noexcept { return values_[static_cast<size_t>(index)].load(std::memory_order_relaxed); }

    // Sanitizes against the spec; returns the stored value. RT-safe (used by the audio thread for ReadOnly).
    float set(int index, float value) noexcept;
    // Audio-thread write for ReadOnly params (no sanitize).
    void setRaw(int index, float value) noexcept { values_[static_cast<size_t>(index)].store(value, std::memory_order_relaxed); }

    bool set(std::string_view id, float value) noexcept;
    void resetToDefaults() noexcept;

    // Monotonic counter bumped on every control-side write; modules may use it to skip recomputation.
    uint32_t version() const noexcept { return version_.load(std::memory_order_acquire); }

private:
    const std::vector<ParamSpec>* specs_;
    int size_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::atomic<uint32_t> version_{0};
};

} // namespace ks
