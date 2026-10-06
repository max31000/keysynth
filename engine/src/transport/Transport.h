#pragma once
// Engine-owned transport clock (ARCHITECTURE §8): tempo, time signature, play/stop, ppq position.
// Control thread writes the atomics; the audio thread advances the position once per block.
// Position = anchorPpq + samplesSinceAnchor * tempo / (60 * sr), re-anchored when the tempo changes, so
// accumulated rounding does not depend on the block size (sample-accurate sequencer steps).

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
    int denominator() const noexcept { return den_.load(std::memory_order_relaxed); }

    // --- audio thread ---
    // Returns the transport state at the start of the block. `started` is true when play just began
    // (position reset to 0), `stopped` when it just ended.
    TransportInfo beginBlock(bool& started, bool& stopped) noexcept;
    TransportInfo beginBlock(bool& started) noexcept {
        bool stopped = false;
        return beginBlock(started, stopped);
    }
    void endBlock(int numSamples, double sampleRate) noexcept;
    void resetPosition() noexcept;
    // After Engine::prepare (device restart): the next playing block counts as a start again.
    void resetPlayState() noexcept { wasPlaying_ = false; }

private:
    std::atomic<double> tempo_{120.0};
    std::atomic<bool> playing_{false};
    std::atomic<int> num_{4}, den_{4};
    std::atomic<double> ppqShared_{0.0};
    // audio thread
    double ppq_ = 0.0;
    double anchorPpq_ = 0.0;
    int64_t sinceAnchor_ = 0;
    double anchorTempo_ = 120.0;
    bool wasPlaying_ = false;
    double blockTempo_ = 120.0;
    bool blockPlaying_ = false;
};

} // namespace ks
