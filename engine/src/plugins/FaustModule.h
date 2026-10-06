#pragma once
// Modules backed by a JIT-compiled Faust factory (ARCHITECTURE §10, docs/PLUGINS.md).
//  - FaustInstrument: own polyphony on top of N Faust instances (one per voice, freq/gain/gate convention,
//    core VoiceAllocator for stealing/sustain). A voice ends when its gate is off and its output stayed below
//    -90 dBFS for 2048 samples (or after max_release_s).
//  - FaustEffect: one stereo instance (2->2, 1->2, 2->1) or two instances for 1->1 (dual mono).
// Params are copied into every instance's zones once per block. compute() runs inside an SEH guard; a fault
// or a non-finite output mutes the module and is reported through PluginVersion::faults.

#include "core/VoiceAllocator.h"
#include "plugins/FaustJit.h"
#include "plugins/PluginVersion.h"

#include <array>
#include <memory>
#include <vector>

namespace ks::plugins {

class FaustPluginVersion final : public PluginVersion {
public:
    std::unique_ptr<Module> createModule() const override;

    std::shared_ptr<FaustFactory> factory;
    FaustMapping mapping;
    int numInputs = 0, numOutputs = 0;
    bool instrument = false;
    int polyphony = 8;
    float maxReleaseSeconds = 10.0f;
    float tailSeconds = 1.0f; // effects
};

// Faust instance + its zones, bound to the module's params.
struct FaustInstance {
    void* dsp = nullptr;
    std::vector<float*> zones;   // per FaustUiDesc control
    std::vector<float*> params;  // per param index (nullptr if not bound)
    float* freq = nullptr;       // voice controls (instruments)
    float* key = nullptr;
    float* gain = nullptr;
    float* vel = nullptr;
    float* gate = nullptr;
};

class FaustModuleBase : public Module {
public:
    explicit FaustModuleBase(std::shared_ptr<const FaustPluginVersion> v);
    ~FaustModuleBase() override;
    bool valid() const noexcept { return valid_; }
    bool faulted() const noexcept { return faulted_; }

protected:
    bool makeInstance(FaustInstance& inst);
    void pushParams(FaustInstance& inst) noexcept; // audio thread
    void pullReadOnly(const FaustInstance& inst) noexcept;
    void fault(uint32_t code) noexcept;
    void bindEnums();

    std::shared_ptr<const FaustPluginVersion> v_;
    std::vector<FaustInstance> instances_;
    std::vector<const std::vector<float>*> enumValues_; // per param: Enum index -> Faust value (or null)
    double sampleRate_ = 48000.0;
    int maxBlock_ = 0;
    bool valid_ = true;
    bool faulted_ = false;
};

class FaustInstrument final : public FaustModuleBase {
public:
    static constexpr int kMaxVoices = 32;
    explicit FaustInstrument(std::shared_ptr<const FaustPluginVersion> v);

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;
    int activeVoices() const override { return active_.load(std::memory_order_relaxed); }

    struct Shared;
    struct Voice {
        Shared* shared = nullptr;
        FaustInstance* inst = nullptr;
        int note = 60;
        bool active = false, released = false, retrigger = false;
        int silentSamples = 0;
        int64_t releasedSamples = 0;

        void noteOn(const VoiceStart& s) noexcept;
        void noteOff() noexcept;
        void kill() noexcept;
        void reset() noexcept;
        bool isActive() const noexcept { return active; }
        void setPitch(float bendSemis) noexcept;
        void render(AudioBlock& out, int start, int n) noexcept;
    };
    struct Shared {
        float bendSemis = 0.0f;
        int64_t maxReleaseSamples = 0;
        bool stereo = false;
        std::vector<float> l, r; // per-voice scratch (maxBlock)
    };

private:
    static void guardedProcess(void* self);
    void processBody() noexcept;

    Shared shared_;
    VoiceAllocator<Voice, kMaxVoices> alloc_;
    std::atomic<int> active_{0};
    // guardedProcess arguments
    AudioBlock* curOut_ = nullptr;
    MidiEventSpan curEvents_;
    const ProcessContext* curCtx_ = nullptr;
};

class FaustEffect final : public FaustModuleBase {
public:
    explicit FaustEffect(std::shared_ptr<const FaustPluginVersion> v);
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

private:
    static void guardedProcess(void* self);
    void processBody() noexcept;
    std::vector<float> inL_, inR_;
    AudioBlock* cur_ = nullptr;
};

} // namespace ks::plugins
