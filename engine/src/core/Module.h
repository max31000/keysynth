#pragma once
// Module API (ARCHITECTURE §5.2). Instruments and effects implement this; everything else talks to them only
// through it.

#include "core/AudioBlock.h"
#include "core/MidiEvent.h"
#include "core/ParamSet.h"
#include "core/ProcessContext.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace ks {

enum class ModuleKind { Instrument, Effect };

struct ModuleInfo {
    std::string typeId;      // lowercase, stable (preset format)
    std::string displayName;
    ModuleKind kind = ModuleKind::Instrument;
    std::string category;    // "Synth", "Utility", "Dynamics", ...
    std::vector<ParamSpec> params;
    nlohmann::json uiHints = nlohmann::json::object(); // optional: groups order, front-panel params, ...
};

const char* toString(ModuleKind k) noexcept;
nlohmann::json toJson(const ModuleInfo& info);

class Module {
public:
    // `info` must outlive the module (modules use a function-local static ModuleInfo).
    explicit Module(const ModuleInfo& info) : info_(&info), params_(info.params) {
        liveCount().fetch_add(1, std::memory_order_relaxed);
    }
    virtual ~Module() { liveCount().fetch_sub(1, std::memory_order_relaxed); }
    // Number of Module instances alive in the process (leak checks in tests).
    static int liveInstances() noexcept { return liveCount().load(); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    virtual const ModuleInfo& info() const { return *info_; }
    ParamSet& params() noexcept { return params_; }
    const ParamSet& params() const noexcept { return params_; }

    // Control thread, before going live. Allocate everything here.
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    // Kill voices / tails. Called on the control thread while not live, or on the audio thread (must be RT-safe).
    virtual void reset() = 0;
    // Audio thread. Instrument: buffer cleared on entry. Effect: in-place stereo. numSamples <= maxBlock.
    // Events are sorted by sampleOffset and lie within [0, numSamples).
    virtual void process(AudioBlock& stereo, MidiEventSpan events, const ProcessContext& ctx) = 0;

    virtual int tailSamples() const { return 0; }
    virtual int latencySamples() const { return 0; }
    // Active voice count for telemetry (instruments). Audio thread writes, any thread reads (approximate).
    virtual int activeVoices() const { return 0; }

    virtual nlohmann::json saveState() const { return {}; } // non-param state (sample path, syx bank, ...)
    virtual void loadState(const nlohmann::json&) {}        // control thread, before prepare

    // Heavy resources (samples, ...) loaded asynchronously after prepare(): false while still loading (the module
    // renders silence meanwhile). Any thread. OfflineRenderer waits until every module is ready (ARCHITECTURE §5.2).
    virtual bool isReady() const { return true; }
    // Control thread, after prepare(), before going live: true when rendered faster than real time (ks-render,
    // tests). Streaming modules may then block on disk IO inside process() instead of dropping audio.
    virtual void setOfflineMode(bool /*offline*/) {}

private:
    static std::atomic<int>& liveCount() noexcept {
        static std::atomic<int> c{0};
        return c;
    }
    const ModuleInfo* info_;
    ParamSet params_;
};

} // namespace ks
