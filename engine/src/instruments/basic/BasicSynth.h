#pragma once
// `basic`: minimal polyphonic subtractive synth used to exercise the core (VoiceAllocator, params, graph).
// Per voice: PolyBLEP saw/square -> TPT SVF low-pass -> ADSR amp. Glide, mono/legato via the allocator.

#include "core/Module.h"
#include "core/VoiceAllocator.h"
#include "dsp/Adsr.h"
#include "dsp/PolyBlep.h"
#include "dsp/Svf.h"

#include <atomic>

namespace ks {

class BasicSynth final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    BasicSynth();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;
    int activeVoices() const override { return activeVoices_.load(std::memory_order_relaxed); }

    // Param indices (order of moduleInfo().params).
    enum P { Wave, Cutoff, Resonance, Attack, Decay, Sustain, Release, VolumeDb, VoiceModeP, Glide, Polyphony, Count };

    struct Shared {
        double sampleRate = 48000.0;
        dsp::Wave wave = dsp::Wave::Saw;
        float cutoff = 3000.0f;
        float resonance = 0.2f;
        float glideCoef = 1.0f;  // one-pole coefficient per control tick
        float bendSemis = 0.0f;
        float gain = 0.2f;
        dsp::Adsr::Params env;
    };

    struct Voice {
        const Shared* shared = nullptr;
        dsp::PolyBlepOsc osc;
        dsp::Svf svf;
        dsp::Adsr env;
        float note = 60.0f;       // current (gliding) pitch
        float targetNote = 60.0f;
        float velGain = 1.0f;
        int tick = 0;

        void noteOn(const VoiceStart& s) noexcept;
        void noteOff() noexcept { env.noteOff(); }
        void kill() noexcept { env.kill(); }
        void reset() noexcept;
        bool isActive() const noexcept { return env.isActive(); }
        void render(float* out, int n) noexcept;
    };

    static constexpr int kMaxVoices = 16;
    VoiceAllocator<Voice, kMaxVoices>& allocator() noexcept { return alloc_; }

private:
    void updateShared(const ProcessContext& ctx) noexcept;
    void renderSegment(AudioBlock& out, int start, int end) noexcept;

    Shared shared_;
    VoiceAllocator<Voice, kMaxVoices> alloc_;
    std::atomic<int> activeVoices_{0};
    VoiceMode mode_ = VoiceMode::Poly;
};

} // namespace ks
