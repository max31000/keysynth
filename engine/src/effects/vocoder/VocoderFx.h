#pragma once
// `vocoder`: classic channel vocoder (Kraftwerk, ELO, Daft Punk). Carrier = the audio arriving at the effect (the
// layer's synth); modulator = the device input bus (ProcessContext::input, ARCHITECTURE §6 "Audio input": a mic on
// the interface) or an internal source.
//   Analysis: `bands` (8/16/20/32) log-spaced 4th-order band-passes (two cascaded unit-peak SVF stages) between
//   kLowHz and kHighHz on the modulator, each followed by an attack/release envelope follower.
//   Synthesis: the same bank on the carrier (L and R), centres shifted by `formant` semitones, each band scaled by
//   its modulator envelope, summed.
//   Sibilance: the modulator's energy above ~5 kHz (consonants: s, t, ch) gates high-passed noise plus a little of
//   the modulator's own top band ("HF pass-through"), scaled by `sibilance` and by the carrier level (silent when
//   the synth is silent, so the tail stays honest).
//   Modulator: `modulator` Input (falls back to Vowels when no input channel is open), Vowels (internal sung
//   vowel sequence a-e-i-o-u with an "s" now and then: offline renders, tests, auditioning without a mic), Noise.
//   `input_channel` picks Left / Right / Mono (L+R)/2 of the bus; `mod_gain_db` and `gate_db` (envelopes close
//   below the gate) condition it. `carrier_mix` adds the unprocessed carrier to the wet, `level_db` trims the wet.
// Zero latency; tail = the carrier band-pass ring.

#include "core/Module.h"
#include "dsp/EnvelopeFollower.h"
#include "dsp/Noise.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"
#include "dsp/TptFilters.h"

#include <array>

namespace ks {

class VocoderFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    VocoderFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P {
        Bands, Modulator, InputChannel, ModGainDb, GateDb, AttackMs, ReleaseMs, Formant, Sibilance, CarrierMix,
        LevelDb, Mix, Count
    };
    enum ModulatorId { ModInput, ModVowels, ModNoise };
    enum InputId { InLeft, InRight, InMono };
    static constexpr int kMaxBands = 32;
    static constexpr float kLowHz = 120.0f, kHighHz = 7000.0f;

    static int bandCount(int bandsIndex) noexcept;
    // Analysis centre of band b for the current band count (tests).
    float bandHz(int b) const noexcept;
    int activeBands() const noexcept { return nBands_; }
    // Current modulator envelope of band b (tests: band energy follows the modulator spectrum).
    float bandEnvelope(int b) const noexcept { return env_[static_cast<size_t>(b)].value(); }

private:
    struct Bp4 { // 4th-order band-pass: two unit-peak SVF stages
        dsp::Svf s1, s2;
        float k = 1.0f;
        void set(float g, float kk) noexcept {
            k = kk;
            s1.setGK(g, kk);
            s2.setGK(g, kk);
        }
        void reset() noexcept {
            s1.reset();
            s2.reset();
        }
        float process(float x) noexcept { return k * s2.tick(k * s1.tick(x).bp).bp; }
    };
    void configureBands(int n, float formantSemis) noexcept; // a band-count change resets the banks (enum switch)
    void retuneCarrier(float formantSemis) noexcept;          // formant glide: carrier bank only
    float vowelSample() noexcept; // internal test modulator

    double sr_ = 48000.0;
    int nBands_ = 0;
    float configuredFormant_ = 1e9f;
    std::array<Bp4, kMaxBands> mod_{};
    std::array<std::array<Bp4, kMaxBands>, 2> car_{};
    std::array<dsp::EnvelopeFollower, kMaxBands> env_{};
    std::array<float, kMaxBands> centreHz_{};
    float bandGain_ = 1.0f;
    float lastAttack_ = -1.0f, lastRelease_ = -1.0f;

    // sibilance
    dsp::Svf modHf_, noiseHp_[2];
    dsp::EnvelopeFollower hfEnv_, gateEnv_, carEnv_;
    dsp::WhiteNoise noise_{0x51b1u}, modNoise_{0x7a11u};
    float gate_ = 0.0f, gateCoef_ = 0.0f;

    // internal vowel source
    double vPhase_ = 0.0, vTime_ = 0.0;
    dsp::Svf formant_[3];
    dsp::TptOnePole glottalLp_, sHp_;
    dsp::OnePoleSmoother f1_, f2_, f3_;

    dsp::OnePoleSmoother formantSm_, modGain_, sib_, carMix_, level_, mix_;
};

} // namespace ks
