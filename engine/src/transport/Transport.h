#pragma once
// Engine-owned transport clock (ARCHITECTURE §8): tempo, time signature, play/stop, ppq position.
// Control thread writes the atomics; the audio thread advances the position once per block.

#include "core/ProcessContext.h"

#include <atomic>
#include <cstdint>

namespace ks {

class Transport {
public:
    // --- control thread ---
    void setTempo(double bpm) noexcept;
    void setPlaying(bool p) noexcept { playing_.store(p, std::memory_order_relaxed); }
    void setTimeSignature(int num, int den) noexcept;
    double tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }
    bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }
    double ppqApprox() const noexcept { return ppqShared_.load(std::memory_order_relaxed); }
    int numerator() const noexcept { return num_.load(std::memory_order_relaxed); }

    // --- audio thread ---
    // Returns the transport state at the start of the block. `started` is true when play just began
    // (position reset to 0).
    TransportInfo beginBlock(bool& started) noexcept;
    void endBlock(int numSamples, double sampleRate) noexcept;
    void resetPosition() noexcept { ppq_ = 0.0; }

private:
    std::atomic<double> tempo_{120.0};
    std::atomic<bool> playing_{false};
    std::atomic<int> num_{4}, den_{4};
    std::atomic<double> ppqShared_{0.0};
    // audio thread
    double ppq_ = 0.0;
    bool wasPlaying_ = false;
    double blockTempo_ = 120.0;
    bool blockPlaying_ = false;
};

} // namespace ks
