#pragma once
// Audio -> control telemetry (ARCHITECTURE §5.5).
//   Continuous values: latest-value atomics (peaks are "max since last poll": the audio thread raises them, the
//   control thread reads-and-clears with exchange at <= 30 Hz).
//   Discrete events (note activity, xruns, ...): SPSC queue; dropped when full, never blocks.

#include "core/Patch.h"
#include "core/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ks {

struct TelemetryEvent {
    enum class Type : uint8_t { NoteOn, NoteOff, Overload };
    Type type = Type::NoteOn;
    uint8_t note = 0;
    uint8_t velocity = 0;
    uint8_t channel = 0;
};

class Telemetry {
public:
    static constexpr int kMaxLayers = 16;

    struct LayerMeter {
        std::atomic<NodeId> node{0};
        std::atomic<float> l{0.0f}, r{0.0f};
    };

    // audio thread
    static void raise(std::atomic<float>& a, float v) noexcept {
        if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
    }
    void pushEvent(const TelemetryEvent& e) noexcept {
        if (!events.push(e)) dropped.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<float> cpuLoad{0.0f}; // max callback time / buffer duration since last poll (0..1+)
    std::atomic<float> masterL{0.0f}, masterR{0.0f};
    std::atomic<int> voices{0};
    std::atomic<int> layerCount{0};
    std::array<LayerMeter, kMaxLayers> layers{};
    std::atomic<uint64_t> overloads{0}; // callbacks that took longer than the buffer duration
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> blocks{0};
    std::atomic<double> ppq{0.0};
    std::atomic<double> limiterReductionDb{0.0};
    SpscQueue<TelemetryEvent> events{4096};
};

} // namespace ks
