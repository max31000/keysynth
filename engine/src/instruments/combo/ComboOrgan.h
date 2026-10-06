#pragma once
// `combo`: transistor combo organ (ARCHITECTURE §7) with Vox Continental and Farfisa Compact voicings.
//
// Divide-down generator: 12 master oscillators (one per semitone class, slightly mistuned like real LC
// oscillators, vibrato applied to the masters) and flip-flop dividers. Each (class, octave) divider output is
// a phase-locked square (octave k phase = master phase << k, exact like the hardware), band-limited with
// PolyBLEP. Keys add weights onto (class, octave) sources per voice bus, ramped every 16 samples, so cost
// scales with sounding sources, not keys.
//   Vox: footage drawbars 16' 8' 4' 2' + IV mixture (2 2/3' 2' 1 3/5' 1'), tone drawbars "~" (flute: sine-ish)
//        and "^" (reed: divider staircase = sum of octave squares ~ ramp) -> bright reed filter.
//   Farfisa: tabs flute 16'/8'/4' (square -> low-pass formant), oboe 8' (square -> nasal band-pass), trumpet 8'
//        (staircase -> brassy band-pass), strings 8'/4' (staircase -> high-pass buzz).
//   Bass section: keys below `bass_split` play only the bass voice (16'/8' squares -> low-pass).
// Output: bus sum -> bright/mellow tone filter -> expression (CC11) -> volume. Mono (L = R).

#include "core/Module.h"
#include "dsp/PhaseTable.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ks {

class ComboOrgan final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    ComboOrgan();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override { return static_cast<int>(0.05 * sampleRate_); }
    int activeVoices() const override { return activeKeys_.load(std::memory_order_relaxed); }

    enum P {
        Voicing,
        Vox16, Vox8, Vox4, Vox2, VoxIV, VoxFlute, VoxReed,
        FarFlute16, FarFlute8, FarFlute4, FarOboe8, FarTrumpet8, FarStrings8, FarStrings4,
        Bass, BassSplit, Bass16, Bass8,
        VibratoOn, VibratoRate, VibratoDepth, Bright, Leakage, Click, Velocity, VolumeDb,
        Count
    };

    static constexpr int kClasses = 12;
    static constexpr int kOctaves = 10;          // octave 0 = C0 (MIDI 12) .. octave 9
    static constexpr int kSources = kClasses * kOctaves;
    static constexpr int kChunk = 16;
    // Busses fed by square sources (the Vox flute bus is fed by sines).
    enum Bus { VoxReedBus, FluteBus, OboeBus, TrumpetBus, StringsBus, BassBus, kSqBusses };

    // Master oscillator detune in cents (fixed, per class) — exposed for tests.
    static float masterDetuneCents(int cls) noexcept;

private:
    void handleEvent(const MidiEvent& e) noexcept;
    void updateBlockParams(const ProcessContext& ctx) noexcept;
    void renderChunk(float* dst, int len) noexcept;
    void addPitch(int midiPitch, int bus, float w) noexcept;     // square source onto a bus
    void addSine(int midiPitch, float w) noexcept;               // sine source (Vox flute)
    void addStair(int midiPitch, int bus, float w) noexcept;     // divider staircase (ramp-ish)
    int sourceFor(int midiPitch) const noexcept;                 // -1 if out of range

    double sampleRate_ = 48000.0;
    dsp::PhaseTable sine_;
    std::array<uint32_t, kClasses> phase_{};
    std::array<double, kClasses> baseInc_{};
    std::array<uint32_t, kClasses> inc_{};
    std::array<int, kClasses> maxOctave_{};       // highest octave below the fold limit

    std::array<std::array<float, kSqBusses>, kSources> wCur_{}, wTgt_{};
    std::array<float, kSources> sCur_{}, sTgt_{};

    std::array<bool, 128> down_{};
    std::array<float, 128> gate_{};
    std::array<float, 128> vel_{};

    // Block params.
    bool vox_ = true;
    float voxFoot_[4] = {};
    float voxIV_ = 0.0f, voxFlute_ = 0.0f, voxReed_ = 1.0f;
    bool far_[7] = {};
    bool bass_ = false;
    int bassSplit_ = 60;
    float bass16_ = 0.0f, bass8_ = 0.0f;
    float vibDepthSemis_ = 0.0f;
    float vibInc_ = 0.0f;
    float vibPhase_ = 0.0f;
    float leak_ = 0.0f;
    float velSens_ = 0.0f;
    float attackStep_ = 1.0f, releaseStep_ = 1.0f; // per sample
    bool bright_ = true;
    int brightApplied_ = -1; // tone filter state last applied (-1 = never)

    std::array<dsp::Svf, 9> filt_; // bus formants (+ second stages) and master tone
    dsp::OnePoleSmoother outGain_;
    std::atomic<int> activeKeys_{0};
};

} // namespace ks
