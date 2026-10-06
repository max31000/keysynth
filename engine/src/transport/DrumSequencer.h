#pragma once
// DrumSequencer (ARCHITECTURE §8): step sequencer driving the RhythmNode's `drums` module.
//   - control thread: setPattern() copies an RtPattern into a preallocated triple buffer (no allocation, no locks,
//     never blocks); the audio thread picks up the newest one at the start of a block.
//   - audio thread: start()/stop() on transport edges, generate() appends the note-ons of every step whose exact
//     time falls inside the block (sample offsets relative to the block start).
// Step time (quarter notes): origin + rel * stepPpq (+ swing * stepPpq / 2 for odd rel), rel = step - anchorStep.
// A step fires at the first sample >= its exact time, so onsets do not depend on the block size.

#include "core/MidiEvent.h"
#include "core/ProcessContext.h"
#include "transport/Pattern.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ks {

class DrumSequencer {
public:
    static constexpr int kChannel = 10;
    static constexpr int kMaxStepsPerBlock = 64; // bound on work per block (ppq jumps)

    DrumSequencer();

    // --- control thread ---
    void setPattern(const RtPattern& p) noexcept;

    // --- audio thread ---
    // Transport started: first step (index 0) at `originPpq` (> 0 for a count-in).
    void start(double originPpq) noexcept;
    void stop() noexcept;
    bool running() const noexcept { return running_; }
    // Appends at most `cap` events to `out` (sorted by sampleOffset). Returns the number written.
    // `swing` 0..1; `emit` false = advance silently (drums disabled).
    int generate(const TransportInfo& t, int numSamples, double sampleRate, float swing, bool emit, MidiEvent* out,
                 int cap) noexcept;
    // Pick up a newer pattern now (also done by generate()).
    void syncPattern() noexcept;
    const RtPattern& activePattern() const noexcept { return buf_[static_cast<size_t>(front_)]; }

    // --- any thread (approximate, for telemetry) ---
    int currentStep() const noexcept { return currentStep_.load(std::memory_order_relaxed); }

private:
    static constexpr int kDirty = 4;
    std::array<RtPattern, 3> buf_{};
    int back_ = 0;                // control thread
    std::atomic<int> middle_{1};  // index | kDirty
    int front_ = 2;               // audio thread

    // audio thread
    bool running_ = false;
    int64_t nextStep_ = 0;   // absolute index of the next step to fire
    int64_t anchorStep_ = 0; // step whose unswung time is anchorPpq_
    double anchorPpq_ = 0.0;
    std::atomic<int> currentStep_{-1};
};

} // namespace ks
