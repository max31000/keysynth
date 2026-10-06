#pragma once
// `drums`: synthesized drum machine (ARCHITECTURE §7). 13 instruments, each with a model (TR-808, TR-909,
// LinnDrum-like, "Industrial") taken from the `kit` param or overridden per instrument (`<v>_model`), and
// level / tune / decay / tone / pan params. Velocity sensitive; closed/pedal hat chokes the open hat; retriggering
// an instrument fades its previous hit (4 ms). Keys-playable: GM drum map on 35..59 (+ a few percussion notes),
// every other key folds onto 36..47 by pitch class (kick, rim, snare, clap, ..., open hat, tom).
// Used by layers (keys) and by the RhythmNode (DrumSequencer). Note-offs are ignored (one-shots).

#include "core/Module.h"
#include "dsp/Smoother.h"
#include "instruments/drums/DrumVoice.h"

#include <array>
#include <atomic>

namespace ks {

class DrumKit final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    DrumKit();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override { return static_cast<int>(2.0 * sampleRate_); }
    int activeVoices() const override { return activeVoices_.load(std::memory_order_relaxed); }

    // Global param indices; per-instrument params follow at kVoiceBase + instr * kPerVoice + VoiceParam.
    enum Global { Kit, VolumeDb, VelocitySens, kGlobalCount };
    enum VoiceParam { VModel, VLevel, VTune, VDecay, VTone, VPan, kPerVoice };
    static constexpr int kVoiceBase = kGlobalCount;
    static int paramIndex(drums::Instr i, VoiceParam p) noexcept {
        return kVoiceBase + static_cast<int>(i) * kPerVoice + static_cast<int>(p);
    }

    struct NoteMapping {
        drums::Instr instr = drums::Instr::Kick;
        float tuneSt = 0.0f;
        float level = 1.0f;
        float decayMul = 1.0f;
        bool rideBell = false;
    };
    static NoteMapping mapNote(int note) noexcept;

    static constexpr int kSlotsPerInstr = 3;

    // Tests / introspection (audio thread or offline).
    const drums::DrumVoice& voice(drums::Instr i, int slot) const noexcept {
        return voices_[static_cast<size_t>(static_cast<int>(i) * kSlotsPerInstr + slot)];
    }

private:
    void trigger(int note, float velocity) noexcept;
    void renderSegment(AudioBlock& out, int start, int end) noexcept;

    double sampleRate_ = 48000.0;
    std::array<drums::DrumVoice, drums::kNumInstr * kSlotsPerInstr> voices_{};
    uint32_t seed_ = 0x2545F491u;
    dsp::OnePoleSmoother volume_;
    // DC blocker (asymmetric one-shot envelopes leave a small offset)
    float dcR_ = 0.999f, dcX_[2] = {0.0f, 0.0f}, dcY_[2] = {0.0f, 0.0f};
    std::atomic<int> activeVoices_{0};
};

} // namespace ks
