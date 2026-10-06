#pragma once
// `organ`: Hammond B-3 style tonewheel organ (ARCHITECTURE §7, design notes in ToneWheelOrgan.cpp).
//
// Wheel-bus architecture: 91 tonewheel oscillators (real gear ratios, 20 rps shaft) always run; each of the
// 61 keys (C2..C7) has 9 contacts that connect one wheel each to the 9 drawbar busses. Every 16-sample chunk
// the per-wheel gain = sum over closed contacts of drawbar level (+ percussion, + leakage) is recomputed and
// ramped linearly, so cost depends on the number of sounding wheels (<= 91), not on keys held.
// Signal: wheels -> (key click noise) -> scanner vibrato/chorus -> preamp (optional tube drive) -> expression
// (CC11) -> volume. Mono (L = R); pair with the `rotary` effect for stereo.

#include "core/Module.h"
#include "dsp/PhaseTable.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"
#include "dsp/TubeStage.h"
#include "instruments/organ/ScannerVibrato.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ks {

class ToneWheelOrgan final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    ToneWheelOrgan();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override { return static_cast<int>(0.05 * sampleRate_); }
    int activeVoices() const override { return activeKeys_.load(std::memory_order_relaxed); }

    enum P {
        Db16, Db513, Db8, Db4, Db223, Db2, Db135, Db113, Db1,
        Perc, PercHarmonic, PercDecay, PercVolume,
        Vibrato, Click, Leakage, Drive, VolumeDb,
        Count
    };

    static constexpr int kWheels = 91;  // wheel numbers 1..91
    static constexpr int kKeys = 61;    // C2 (MIDI 36) .. C7 (MIDI 96)
    static constexpr int kLowNote = 36;
    static constexpr int kBars = 9;
    static constexpr int kChunk = 16;   // samples per gain-update chunk

    // Tonewheel frequency in Hz (1..91) for the 1200 rpm synchronous motor (60 Hz mains).
    static double wheelFrequency(int wheel) noexcept;
    // Wheel number connected to drawbar `bar` (0 = 16' .. 8 = 1') of a MIDI note (folded into C2..C7),
    // including the manual's foldback (bottom octave of 16', top of the high footages).
    static int wheelFor(int midiNote, int bar) noexcept;
    // Drawbar position 0..8 -> linear gain (~3 dB per step, 8 = 1.0).
    static float drawbarGain(float position) noexcept;

    // Test/diagnostic hooks (audio thread state, read when not processing).
    float percussionLevel() const noexcept { return percEnv_; }
    int percussionTriggers() const noexcept { return percTriggers_; }

private:
    struct Key {
        bool down = false;
        bool active = false;  // any contact still (partly) closed
        int t = 0;            // samples since last press/release
        uint32_t bounce = 0;  // random bits for contact bounce
        float v[kBars] = {};  // contact closure 0..1
        int delay[kBars] = {}; // contact make/break time after the key event (samples)
    };

    void handleEvent(const MidiEvent& e) noexcept;
    void keyDown(int key) noexcept;
    void keyUp(int key) noexcept;
    void renderChunk(float* dst, int len) noexcept;
    void updateBlockParams(const ProcessContext& ctx) noexcept;

    double sampleRate_ = 48000.0;
    std::array<dsp::PhaseTable, 4> tables_;
    // Index 0 unused so wheel numbers index directly.
    std::array<uint32_t, kWheels + 1> phase_{};
    std::array<uint32_t, kWheels + 1> inc_{};
    std::array<float, kWheels + 1> wheelGain_{};
    std::array<uint8_t, kWheels + 1> wheelTable_{};
    std::array<float, kWheels + 1> cur_{};
    std::array<float, kWheels + 1> target_{};
    std::array<float, kWheels + 1> keyed_{};
    std::array<std::array<uint8_t, kBars>, kKeys> wheelMap_{};
    std::array<Key, kKeys> keys_{};
    std::array<bool, 128> noteDown_{};     // MIDI notes held (several fold onto one key)
    std::array<uint8_t, kKeys> holds_{};   // held MIDI notes per manual key
    int keysDown_ = 0;

    // Block parameters.
    float barGain_[kBars] = {};
    float barTotal_ = 0.0f;
    bool percOn_ = false;
    int percBar_ = Db4;
    float percTau_ = 0.15f;
    float percGain_ = 1.0f;
    float click_ = 0.5f;
    float leak_ = 0.3f;
    float drive_ = 0.0f;
    int rampSamples_ = 16;
    int spreadSamples_ = 0;
    int bounceSamples_ = 0;

    float percEnv_ = 0.0f;
    int percTriggers_ = 0;

    // Key click noise (contact bounce) burst.
    dsp::XorNoise noise_;
    float clickEnv_ = 0.0f;
    float clickDecay_ = 0.99f;
    dsp::Svf clickFilter_;

    ScannerVibrato scanner_;
    dsp::TubeStage preamp_;
    dsp::OnePoleSmoother outGain_;
    std::atomic<int> activeKeys_{0};
};

} // namespace ks
