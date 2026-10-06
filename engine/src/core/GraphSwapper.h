#pragma once
// GraphSwapper (ARCHITECTURE §5.4): hands RackGraphs from the control thread to the audio thread.
//   - single `pending` atomic slot; publish() exchanges into it and deletes a graph the audio thread never took
//   - the audio thread takes `pending` only when no transition is running -> at most 2 live graphs
//   - transitions: if the graphs share modules -> 20 ms crossfade, shared modules rendered once per block
//     (RenderCache); if nothing is shared -> old graph gets AllNotesOff and keeps rendering until silent or
//     min(max(tail, 50 ms), 1 s), then a 10 ms fade; a new pending graph during that forces a 10 ms fade-out
//   - retire: old graph -> SPSC (capacity 8; on failure kept and retried) -> control thread deletes it

#include "core/RackGraph.h"
#include "core/SpscQueue.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace ks {

class GraphSwapper {
public:
    GraphSwapper();
    ~GraphSwapper();
    GraphSwapper(const GraphSwapper&) = delete;
    GraphSwapper& operator=(const GraphSwapper&) = delete;

    // --- control thread ---
    // Only while the audio thread is not rendering: deletes every graph and resets state.
    void prepare(double sampleRate, int maxBlock);
    void publish(std::unique_ptr<RackGraph> graph);
    // Most recently published graph (pending or live); target for param writes and the diff base.
    RackGraph* latest() const noexcept { return latest_; }
    // Deletes graphs retired by the audio thread. Call periodically (50 ms timer).
    void collectGarbage();
    // Graphs still owned (pending + live + retired-not-collected). For tests.
    size_t ownedCount() const noexcept { return owned_.size(); }

    // --- audio thread ---
    // Renders current (+ old during a transition) into `out` (overwritten). `events` go to the current graph.
    void render(const AudioBlock& out, MidiEventSpan events, const RenderArgs& args) noexcept;
    bool inTransition() const noexcept { return old_ != nullptr; }
    const RackGraph* current() const noexcept { return current_; }
    int activeVoices() const noexcept;
    uint64_t transitionsStarted() const noexcept { return transitions_.load(std::memory_order_relaxed); }

    static constexpr float kCrossfadeSeconds = 0.020f;
    static constexpr float kFadeSeconds = 0.010f;
    static constexpr float kMaxTailSeconds = 1.0f;

private:
    enum class Mode { Crossfade, TailOut };
    void begin(RackGraph* next) noexcept;
    bool finish() noexcept; // false if the retire queue is full (try again next block)
    void forceFade() noexcept;

    // control
    std::vector<std::unique_ptr<RackGraph>> owned_;
    RackGraph* latest_ = nullptr;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;

    // shared
    std::atomic<RackGraph*> pending_{nullptr};
    SpscQueue<RackGraph*> retire_{8};
    std::atomic<uint64_t> transitions_{0};

    // audio
    RackGraph* current_ = nullptr;
    RackGraph* old_ = nullptr;
    std::array<RackGraph*, 8> backlog_{};
    int backlogSize_ = 0;
    RenderCache cache_;
    std::vector<float> oldL_, oldR_;
    Mode mode_ = Mode::Crossfade;
    bool sendAllNotesOff_ = false;
    bool fading_ = false;
    int64_t pos_ = 0;
    int64_t limit_ = 0;
    int64_t fadeLen_ = 0;
    int64_t fadePos_ = 0;
    int64_t xfadeLen_ = 0;
    int64_t silentRun_ = 0;
};

} // namespace ks
