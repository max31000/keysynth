#pragma once
// RackGraph: the live, immutable-structure signal graph rendered by the audio thread (ARCHITECTURE §5.3).
//   LayerNode[] (zone filter -> instrument -> fx chain -> gain/pan -> meters) -> sum -> master fx -> master vol.
// Built on the control thread by GraphBuilder, handed to the audio thread by GraphSwapper. Structure never
// changes after publish; param-like values (zone, bypass, volume) are atomics written by the control thread.
// The metronome and the safety limiter live in the Engine output stage (they persist across graph swaps and
// are applied once to the mixed output during transitions).

#include "core/ChannelState.h"
#include "core/Module.h"
#include "core/Patch.h"
#include "dsp/Smoother.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ks {

inline constexpr int kMaxEventsPerBlock = 512;

// Render-once cache used while two graphs share modules during a transition (GraphSwapper). Preallocated.
class RenderCache {
public:
    static constexpr int kSlots = 16 * 18 + 16; // every module of a maximal patch
    void prepare(int maxBlock);
    void beginBlock() noexcept { ++stamp_; }
    // Returns a slot index for `m` (stable for the transition), or -1 when full.
    int slotFor(const Module* m) noexcept;
    void clear() noexcept;
    void store(int slot, const AudioBlock& b) noexcept;
    bool load(int slot, const AudioBlock& b) const noexcept; // false if not stored this block
private:
    struct Entry {
        const Module* module = nullptr;
        uint64_t stamp = 0;
    };
    std::array<Entry, kSlots> entries_{};
    std::vector<float> storage_;
    int maxBlock_ = 0;
    int used_ = 0;
    uint64_t stamp_ = 1;
};

struct ModuleNode {
    NodeId node = 0;
    std::string type;
    nlohmann::json state;
    std::shared_ptr<Module> module; // may be null (unknown type) -> passthrough / silent
    std::atomic<bool> bypass{false};
    // Audio thread, during a transition: -1 = not shared; >= 0 = render-once cache slot; kSharedNoCache = shared
    // but the cached output is not valid for the old chain (upstream differs) -> the old graph passes through.
    int cacheSlot = -1;
    static constexpr int kSharedNoCache = -2;
};

struct LayerNode {
    NodeId node = 0;
    ModuleNode instrument;
    std::vector<std::unique_ptr<ModuleNode>> fx;

    // Zone (atomics, written by the control thread).
    std::atomic<int> keyLo{0}, keyHi{127}, velLo{1}, velHi{127}, transpose{0}, channel{0};
    std::atomic<float> volumeDb{0.0f}, pan{0.0f};
    std::atomic<bool> mute{false}, solo{false}, sustain{true};
    void setZone(const Zone& z) noexcept;
    Zone zone() const noexcept;

    // Physical (channel,key) -> sounding note + 1 (0 = not sounding). Copied into the next graph by
    // GraphSwapper::begin (audio thread); atomics so the control thread may inspect it.
    std::array<std::atomic<uint8_t>, 16 * 128> noteMap{};

    // Audio-thread scratch.
    std::vector<MidiEvent> events;
    std::vector<float> bufL, bufR;
    dsp::OnePoleSmoother gainL, gainR;
    std::atomic<float> meterL{0.0f}, meterR{0.0f}; // block peak of the latest block

    void filterEvents(MidiEventSpan in, std::vector<MidiEvent>& out) noexcept;
};

// RhythmNode (ARCHITECTURE §5.3): the `drums` module played by the Engine's DrumSequencer. Rendered into its own
// buffer, gain-ramped by the per-block drums volume and summed with the layers before master FX.
struct RhythmNode {
    ModuleNode drums;
    std::vector<float> bufL, bufR;
    float lastGain = -1.0f; // audio thread; < 0 = snap on the first block
};

struct RenderArgs {
    ProcessContext ctx;                  // numSamples, sampleRate, sampleTime, transport; channel ignored
    const ChannelState* channels = nullptr; // 17 states (0 = omni)
    MidiEventSpan rhythmEvents;          // DrumSequencer output for the RhythmNode (sorted, in-block offsets)
    float rhythmGain = 1.0f;             // linear drums volume
    RenderCache* cache = nullptr;
    bool writeCache = false; // new graph during a transition
    bool readCache = false;  // old graph during a transition
};

class RackGraph {
public:
    RackGraph(double sampleRate, int maxBlock);
    ~RackGraph();
    RackGraph(const RackGraph&) = delete;
    RackGraph& operator=(const RackGraph&) = delete;

    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }

    std::vector<std::unique_ptr<LayerNode>> layers;
    std::vector<std::unique_ptr<ModuleNode>> masterFx;
    std::atomic<float> masterVolumeDb{0.0f};
    RhythmNode rhythm;

    // Control-thread lookups (built by finalize()).
    void finalize();
    Module* findModule(NodeId node) const;
    ModuleNode* findModuleNode(NodeId node) const;
    LayerNode* findLayer(NodeId node) const;
    std::vector<ModuleNode*> allModuleNodes() const;

    // Audio thread. `out` is overwritten. RT-safe.
    void render(const AudioBlock& out, MidiEventSpan events, const RenderArgs& args) noexcept;

    // Max tail over all modules (samples), for transitions.
    int maxTailSamples() const noexcept;
    int activeVoices() const noexcept;

    // Audio thread: reset render-once slots.
    void clearCacheSlots() noexcept;

private:
    void processNode(ModuleNode& n, AudioBlock& block, MidiEventSpan events, const ProcessContext& ctx,
                     const RenderArgs& args) noexcept;

    double sampleRate_;
    int maxBlock_;
    dsp::OnePoleSmoother masterGain_;
    // masterFx index where `masterVolumeDb` is applied: before the trailing run of `limiter` slots (the limiter
    // stays last), else after the whole chain (finalize()).
    size_t masterVolumeAt_ = 0;
    std::unordered_map<NodeId, ModuleNode*> moduleIndex_;
    std::unordered_map<NodeId, LayerNode*> layerIndex_;
};

} // namespace ks
