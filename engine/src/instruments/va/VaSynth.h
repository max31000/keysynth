#pragma once
// `va`: virtual analog polysynth (ARCHITECTURE §7). Juno-60/106, Jupiter-8, OB-Xa, Prophet-5, Minimoog,
// CS-80-ish brass and JP-8000 supersaw territory.
//
// Per note (allocator voice, max 16): amp + filter envelope, 2 LFOs (key-synced or global/tempo-synced),
// glide, 6-slot mod matrix, evaluated at control rate (every kControlInterval samples).
// Per unison sub-voice (up to 8 per note, total budget kSubVoiceBudget): 3 band-limited oscillators
// (saw/pulse/tri/sine/supersaw/noise; osc1 can hard-sync to osc2 and take linear FM from osc3), sub square,
// noise, ring mod (osc1*osc2), one-pole HPF, filter (Moog ladder / IR3109 ladder / SEM SVF), VCA, pan.
// Analog drift: per-sub-voice slow random pitch/cutoff wander plus a static calibration offset.
// Output: soft saturation, volume, DC blocker. Zero latency (oscillators carry a 1-sample BLEP look-ahead).
// Zipper-free knobs: continuous mixer/filter params are one-pole smoothed per voice at control rate (snapped on
// a fresh note); filter coefficients, gain and pulse width are additionally ramped per sample.

#include "core/Module.h"
#include "core/VoiceAllocator.h"
#include "dsp/AnalogEnv.h"
#include "dsp/BlepOsc.h"
#include "dsp/LadderFilter.h"
#include "dsp/Noise.h"
#include "dsp/OnePoleTpt.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ks {

class VaSynth final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    VaSynth();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;
    int activeVoices() const override { return activeSubVoices_.load(std::memory_order_relaxed); }

    static constexpr int kMaxVoices = 16;      // notes
    static constexpr int kMaxUnison = 8;       // sub-voices per note
    static constexpr int kSubVoiceBudget = 64; // total sub-voices: polyphony is capped to budget / unison
    static constexpr int kControlInterval = 16;
    static constexpr int kModSlots = 6;
    static constexpr int kNumOscs = 3;

    // Param indices = order of moduleInfo().params. Ids are the preset format: never reorder silently.
    enum P : int {
        // per-osc block (8 params) for osc1..osc3, osc1 has 3 extra after its block
        Osc1Wave, Osc1Octave, Osc1Semi, Osc1Fine, Osc1Level, Osc1Pw, Osc1Pwm, Osc1Detune,
        Osc1Sync, Osc1Fm, Osc1PmEnv,
        Osc2Wave, Osc2Octave, Osc2Semi, Osc2Fine, Osc2Level, Osc2Pw, Osc2Pwm, Osc2Detune,
        Osc3Wave, Osc3Octave, Osc3Semi, Osc3Fine, Osc3Level, Osc3Pw, Osc3Pwm, Osc3Detune,
        SubLevel, SubOctave, NoiseLevel,
        RingLevel, OscReset,
        FilterModel, FilterMode, Cutoff, Resonance, FilterDrive, KeyTrack, FilterEnvAmt, FilterVel, HpfCutoff,
        FenvAttack, FenvDecay, FenvSustain, FenvRelease, FenvVel,
        AmpAttack, AmpDecay, AmpSustain, AmpRelease, AmpVel,
        Lfo1Wave, Lfo1Rate, Lfo1Sync, Lfo1Division, Lfo1Delay, Lfo1KeySync, Lfo1Pitch, Lfo1Cutoff, Lfo1Amp,
        Lfo2Wave, Lfo2Rate, Lfo2Sync, Lfo2Division, Lfo2Delay, Lfo2KeySync,
        Mod1Src, Mod1Dst, Mod1Amt, Mod2Src, Mod2Dst, Mod2Amt, Mod3Src, Mod3Dst, Mod3Amt,
        Mod4Src, Mod4Dst, Mod4Amt, Mod5Src, Mod5Dst, Mod5Amt, Mod6Src, Mod6Dst, Mod6Amt,
        VoiceModeP, Polyphony, Unison, UnisonDetune, UnisonSpread, Glide, GlideMode, Drift, PanSpread,
        BendRange, MwVibrato, MwCutoff, AtVibrato, AtCutoff,
        Drive, VolumeDb,
        Count
    };

    enum class OscType { Saw = 0, Pulse, Triangle, Sine, SuperSaw, Noise };
    enum class FilterType { Moog = 0, Juno, Sem };
    enum ModSrc : int { SrcOff = 0, SrcLfo1, SrcLfo2, SrcFenv, SrcAenv, SrcVelocity, SrcKey, SrcModWheel,
                        SrcAftertouch, SrcBend, SrcExpression, SrcRandom, SrcCount };
    enum ModDst : int { DstOff = 0, DstPitch, DstOsc1Pitch, DstOsc2Pitch, DstOsc3Pitch, DstOsc1Pw, DstOsc2Pw,
                        DstOsc3Pw, DstOsc1Level, DstOsc2Level, DstOsc3Level, DstSubLevel, DstNoiseLevel, DstFm,
                        DstCutoff, DstResonance, DstHpf, DstAmp, DstPan, DstLfo1Depth, DstDetune, DstFenvAmt,
                        DstCount };

    // Per-voice control-rate smoothed params (targets in Shared::smTarget).
    enum Sm : int { SmCutoff = 0, SmResonance, SmDrive, SmLevel1, SmLevel2, SmLevel3, SmPw1, SmPw2, SmPw3, SmSub,
                    SmNoise, SmRing, SmFm, SmHpf, SmCount };

    struct OscShared {
        OscType type = OscType::Saw;
        float pitchOffset = 0.0f; // semitones (octave + semi + fine)
        float level = 1.0f, pw = 0.5f, pwm = 0.0f, detune = 0.5f;
    };

    // Block-rate snapshot of params + controllers, read by voices.
    struct Shared {
        double sampleRate = 48000.0;
        float sr = 48000.0f, invSr = 1.0f / 48000.0f;
        float tickSeconds = kControlInterval / 48000.0f;
        float smCoef = 1.0f;      // per-tick one-pole coefficient of the param smoothers
        float driftCoef = 1.0f;   // per-tick drift wander smoothing
        float driftTickScale = 1.0f; // drift target interval scale (sample-rate independent)
        float smTarget[SmCount] = {};
        float ssCurve[kNumOscs] = {}; // SuperSaw::detuneCurve(osc detune), per block
        int typeEpoch = 0;            // bumped when an osc type changes: voices recompute control at once
        int voiceSubs[kMaxVoices] = {}; // sub-voices in use per voice, refreshed before each event (budget)
        OscShared osc[kNumOscs];
        bool sync = false;
        float fm = 0.0f, pmEnv = 0.0f;
        float subLevel = 0.0f, noiseLevel = 0.0f, ringLevel = 0.0f;
        int subDiv = 2;
        bool phaseReset = false;
        FilterType filter = FilterType::Moog;
        int filterMode = 0;
        float cutoffOct = 13.0f; // log2(Hz)
        float resonance = 0.0f, driveGain = 1.0f, driveComp = 1.0f, keyTrack = 0.5f, fenvAmt = 0.0f,
              filterVel = 0.0f, hpfOct = 3.32f;
        dsp::AnalogEnv::Coefs fenv, aenv;
        float fenvVel = 0.0f, ampVel = 0.5f;
        // LFOs
        int lfoWave[2] = {1, 1};
        bool lfoKeySync[2] = {false, false};
        float lfoDelay[2] = {0.0f, 0.0f};
        double lfoIncPerTick[2] = {0.0, 0.0};   // cycles per control tick (key-synced LFOs)
        double lfoGlobalPhase[2] = {0.0, 0.0};  // at block start
        double lfoGlobalInc[2] = {0.0, 0.0};    // cycles per sample
        float lfo1Pitch = 0.0f, lfo1Cutoff = 0.0f, lfo1Amp = 0.0f;
        // Mod matrix
        int modSrc[kModSlots] = {}, modDst[kModSlots] = {};
        float modAmt[kModSlots] = {};
        // Voice
        float glideSeconds = 0.0f;
        bool glideLegatoOnly = false;
        float unisonDetune = 0.0f, unisonSpread = 0.0f, drift = 0.0f, panSpread = 0.0f;
        int unison = 1;
        float bendSemis = 0.0f, modWheel = 0.0f, aftertouch = 0.0f, expression = 1.0f, bendNorm = 0.0f;
        float mwVibrato = 0.0f, mwCutoff = 0.0f, atVibrato = 0.0f, atCutoff = 0.0f;
        // Set by the module right before a NoteOn is handed to the allocator.
        bool fingeredLegato = false;
        int blockPos = 0; // start of the segment being rendered, inside the block (global LFO phase)
    };

    struct SubVoice {
        dsp::BlepOsc osc[kNumOscs];
        dsp::SuperSaw saw[kNumOscs];
        dsp::BlepOsc sub;
        dsp::WhiteNoise noise;
        dsp::OnePoleTpt hpf;
        dsp::LadderFilter ladder;
        dsp::Svf svf;
        float inc[kNumOscs] = {};
        float gCur = -1.0f, gInc = 0.0f; // filter coefficient, ramped per sample (gCur < 0: snap next tick)
        float panL = 0.7071f, panR = 0.7071f;
        // drift
        float driftStatic = 0.0f, driftCutStatic = 0.0f;
        float driftValue = 0.0f, driftTarget = 0.0f, driftCut = 0.0f, driftCutTarget = 0.0f;
        int driftTicks = 0;
        uint32_t seed = 1;
    };

    struct Voice {
        Shared* shared = nullptr;
        int index = 0;
        dsp::AnalogEnv aenv, fenv;
        std::array<SubVoice, kMaxUnison> subs{};
        int numSubs = 1;
        float note = 60.0f, targetNote = 60.0f, glideStep = 0.0f;
        float velocity = 1.0f, random = 0.0f;
        float timeSinceOn = 0.0f;
        double lfoPhase[2] = {0.0, 0.0};
        uint32_t seed = 1;
        int tickRemain = 0;
        // control-rate results
        float fenvValue = 0.0f;
        float level[kNumOscs] = {}, subLevel = 0.0f, noiseLevel = 0.0f, ringLevel = 0.0f, fm = 0.0f;
        float fbK = 0.0f; // ladder feedback
        float svfK = 2.0f; // SVF damping
        float driveGain = 1.0f, driveComp = 1.0f;
        float pwCur[kNumOscs] = {-1.0f, -1.0f, -1.0f}, pwInc[kNumOscs] = {}; // ramped per sample
        float sm[SmCount] = {};
        bool smSnap = true; // next control tick copies the targets (fresh note)
        FilterType curFilter = FilterType::Moog;
        int typeEpoch = -1;
        float hpfG = 0.0f;
        bool hpfOn = false;
        float gainCur = 0.0f, gainInc = 0.0f; // ramped per sample over the control period
        bool silentHeld = false; // amp env sitting at 0 in sustain: skip DSP

        void noteOn(const VoiceStart& s) noexcept;
        void noteOff() noexcept {
            aenv.noteOff();
            fenv.noteOff();
        }
        void kill() noexcept {
            aenv.kill();
            fenv.kill();
        }
        void reset() noexcept;
        bool isActive() const noexcept { return aenv.isActive(); }
        void render(float* left, float* right, int n) noexcept;

    private:
        void control(int blockPos) noexcept;
        void renderChunk(float* left, float* right, int n) noexcept;
    };

    VoiceAllocator<Voice, kMaxVoices>& allocator() noexcept { return alloc_; }
    int activeSubVoices() const noexcept;

private:
    void updateShared(const ProcessContext& ctx) noexcept;
    void renderSegment(AudioBlock& out, int start, int end) noexcept;

    Shared shared_;
    VoiceAllocator<Voice, kMaxVoices> alloc_;
    std::atomic<int> activeSubVoices_{0};
    VoiceMode mode_ = VoiceMode::Poly;
    std::array<bool, 128> keyDown_{};
    int keysDown_ = 0;
    dsp::OnePoleSmoother volume_, drive_;
    float dcL_ = 0.0f, dcR_ = 0.0f, dcCoef_ = 0.999f;
    double lfoGlobal_[2] = {0.0, 0.0};
};

} // namespace ks
