#pragma once
// Engine: the real-time core. Owns the GraphSwapper, ControlInbox, per-device MIDI queues, global channel
// state, transport + metronome, the output stage (safety limiter) and telemetry.
//
// Threads: prepare()/publish()/collectGarbage()/pushEvent() on the control thread; process()/processBlock() on
// the audio thread (or the offline renderer's thread). See ARCHITECTURE §2, §4, §5.

#include "core/ChannelState.h"
#include "core/GraphSwapper.h"
#include "core/MidiEvent.h"
#include "core/SpscQueue.h"
#include "core/Telemetry.h"
#include "dsp/Limiter.h"
#include "transport/DrumSequencer.h"
#include "transport/Metronome.h"
#include "transport/Transport.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace ks {

class Engine {
public:
    static constexpr int kMaxMidiSources = 16;
    static constexpr int kMaxDeviceBlock = 8192;

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // --- control thread ---
    // Must not be called while process() may run. Drops all graphs: caller publishes a fresh one afterwards.
    void prepare(double sampleRate, int maxBlock);
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }

    GraphSwapper& swapper() noexcept { return swapper_; }
    void publish(std::unique_ptr<RackGraph> g) { swapper_.publish(std::move(g)); }
    RackGraph* latestGraph() const noexcept { return swapper_.latest(); }
    void collectGarbage() { swapper_.collectGarbage(); }

    // ControlInbox (single producer: control thread). Events are delivered at offset 0 of the next block.
    bool pushEvent(const MidiEvent& e) noexcept { return inbox_.push(e); }
    // Per-device MIDI queue (single producer: that device's callback thread). slot in [0, kMaxMidiSources).
    SpscQueue<MidiEvent>& midiSource(int slot) noexcept { return *sources_[static_cast<size_t>(slot)]; }
    void setMidiSourceActive(int slot, bool active) noexcept;

    // All notes off + reset tails (next block).
    void panic() noexcept { panicRequests_.fetch_add(1, std::memory_order_relaxed); }

    Transport& transport() noexcept { return transport_; }
    Metronome& metronome() noexcept { return metronome_; }
    DrumSequencer& sequencer() noexcept { return sequencer_; }

    // Rhythm controls (ARCHITECTURE §8). Control thread writes, audio thread reads once per block.
    struct RhythmControls {
        std::atomic<bool> drumsEnabled{true};
        std::atomic<float> drumsVolume{0.8f}; // linear 0..1
        std::atomic<float> swing{0.0f};       // 0..1
        std::atomic<bool> countIn{false};
    };
    RhythmControls& rhythm() noexcept { return rhythm_; }
    const RhythmControls& rhythm() const noexcept { return rhythm_; }
    // Sequencer step / bar for telemetry (-1 = stopped or counting in).
    int currentStep() const noexcept { return sequencer_.currentStep(); }
    // Song control keys (ARCHITECTURE §18): note-ons of these keys (-1 = none) are removed from the stream and
    // counted; the control thread polls the counters (index 0 = prev, 1 = next).
    void setControlKeys(int prev, int next) noexcept {
        controlKeys_[0].store(prev, std::memory_order_relaxed);
        controlKeys_[1].store(next, std::memory_order_relaxed);
    }
    uint32_t controlKeyPresses(int which) const noexcept {
        return controlPresses_[static_cast<size_t>(which & 1)].load(std::memory_order_relaxed);
    }
    Telemetry& telemetry() noexcept { return telemetry_; }
    int64_t sampleTime() const noexcept { return sampleTimeShared_.load(std::memory_order_relaxed); }

    // --- audio thread ---
    // Device callback: splits into <= maxBlock chunks. Writes channels 0/1 (mono: sum), zeroes the rest.
    void process(float* const* outputs, int numOutputs, int numSamples) noexcept;
    // One block (numSamples <= maxBlock). `extra` are sample-accurate events (offline render), sorted, offsets
    // < numSamples; queued inbox/device events are merged in at offset 0.
    void processBlock(const AudioBlock& out, MidiEventSpan extra = {}) noexcept;

private:
    void gatherEvents(MidiEventSpan extra) noexcept;

    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;

    GraphSwapper swapper_;
    SpscQueue<MidiEvent> inbox_{1024};
    std::array<std::unique_ptr<SpscQueue<MidiEvent>>, kMaxMidiSources> sources_;
    std::array<std::atomic<bool>, kMaxMidiSources> sourceActive_{};
    std::atomic<uint32_t> panicRequests_{0};
    std::array<std::atomic<int>, 2> controlKeys_{-1, -1};
    std::array<std::atomic<uint32_t>, 2> controlPresses_{0u, 0u};
    uint32_t panicSeen_ = 0;

    std::array<ChannelState, 17> channels_{};
    Transport transport_;
    Metronome metronome_;
    DrumSequencer sequencer_;
    RhythmControls rhythm_;
    static constexpr int kMaxRhythmEvents = 256;
    std::array<MidiEvent, kMaxRhythmEvents> rhythmEvents_{};
    double countInEndPpq_ = -1.0;
    bool panicBlock_ = false; // audio thread: panic seen in this block
    dsp::SafetyLimiter limiter_;
    Telemetry telemetry_;

    // audio-thread scratch
    std::vector<MidiEvent> events_;
    std::vector<float> scratchL_, scratchR_;
    int64_t sampleTime_ = 0;
    std::atomic<int64_t> sampleTimeShared_{0};
};

} // namespace ks
