#pragma once
// `sampler`: SFZ instrument built on sfizz (ARCHITECTURE §7). State `{ "sfz": "<path>" }` (docs/PRESETS.md; path
// resolution in SamplePaths.h).
//
// Threading:
//   - prepare() (control thread) only starts a per-instance loader thread; the sfizz instance is created and the
//     SFZ loaded there. Until loading finishes process() outputs silence, `loading` = 1, isReady() = false.
//   - Loads are serialized across all instances (one sfizz_load_file at a time) and skipped when the module is
//     gone before its load starts.
//   - The loader thread stays alive for structural sfizz calls that are not RT-safe (polyphony change: voice
//     reallocation). It pauses the audio thread's access with a lock-free handshake (state + busy flag,
//     seq_cst Dekker-style), so process() never waits.
//   - The sfizz instance (and its sample pool) lives in a Core shared by the module and the loader thread; a module
//     destroyed while a load is still running detaches the thread, which frees the Core when the load returns
//     (the control thread never blocks on disk IO).
//   - GraphBuilder reuses the module when {nodeId, type, state} is unchanged, so FX edits never reload samples.

#include "core/Module.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

struct sfizz_synth_t; // sfizz.h (C API), kept out of this header

namespace ks {

class SamplerModule final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    SamplerModule();
    ~SamplerModule() override;

    void loadState(const nlohmann::json& state) override;
    nlohmann::json saveState() const override;
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;
    int activeVoices() const override { return activeVoices_.load(std::memory_order_relaxed); }
    bool isReady() const override;
    void setOfflineMode(bool offline) override;

    enum P { VolumeDb, Pan, Transpose, Tune, Polyphony, VelocityCurve, Loading, LoadedRegions, Count };
    static constexpr int kMaxPolyphony = 256;
    static constexpr int kPreloadFrames = 16384; // per sample, ~0.34 s at 48 kHz; the rest streams from disk

    // Diagnostics / tests: control thread (they read the core pointer prepare() replaces), or any thread between
    // prepare() calls.
    static int totalLoads() noexcept;  // completed sfizz_load_file calls in this process
    int regionCount() const noexcept;  // 0 while loading or on failure
    bool loadFailed() const noexcept;  // finished loading without any region
    std::string lastError() const;     // control thread
    std::string loadError() const override { return lastError(); }
    std::string resolvedPath() const;  // control thread
    double loadSeconds() const noexcept; // wall time of the last load (0 until finished)
    // Tests: artificial delay before each load starts, so the loading state can be observed deterministically.
    static void setTestLoadDelayMs(int ms) noexcept;
    // Tests: delay after sfizz_load_file returns (the load cannot be cut short), and live Core count.
    static void setTestPostLoadDelayMs(int ms) noexcept;
    static int liveCores() noexcept;

    struct Core; // shared with the loader thread (SamplerModule.cpp)

private:
    void stopCore() noexcept;
    void sendCc(sfizz_synth_t* s, int delay, int cc, float value) noexcept; // audio thread; tracks sent_

    // Controller values sfizz last received (audio thread). -1 = never sent (instrument's own default).
    struct SentControllers {
        float sustain = 0.0f, sostenuto = 0.0f, mod = -1.0f, expression = -1.0f, bend = 0.0f;
    };
    SentControllers sent_;

    std::string sfz_;                // as given in the state
    std::shared_ptr<Core> core_;     // control thread only; audio thread uses coreRaw_
    Core* coreRaw_ = nullptr;
    std::thread loader_;
    bool offline_ = false;
    double sampleRate_ = 0.0;
    int maxBlock_ = 0;

    // Audio-thread state.
    float gainL_ = 1.0f, gainR_ = 1.0f; // smoothed per block (linear ramp inside the block)
    float appliedTune_ = 0.0f;
    bool appliedOffline_ = false;
    bool synced_ = false;               // controller state pushed after (re)gaining access
    uint8_t sentNote_[128] = {};        // incoming note -> transposed note + 1 (0 = not sounding)
    std::atomic<int> activeVoices_{0};
};

} // namespace ks
