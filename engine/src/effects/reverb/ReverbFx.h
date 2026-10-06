#pragma once
// `reverb`: algorithmic reverb, zero latency (pre-delay is a user parameter, 0 allowed).
//   input -> pre-delay -> early reflections (8 taps / channel, size-scaled)
//                      -> 4 Schroeder all-pass diffusers / channel
//                      -> 16-line FDN, fast Walsh-Hadamard feedback matrix (orthogonal, lossless),
//                         per-line slow quadrature-oscillator delay modulation (breaks up modal ringing),
//                         per-line absorption = gain for the mid RT60 (`decay`) + HF and LF shelves derived
//                         from `damp_hf` / `damp_lf`, so RT60 is exact per band (Jot)
//   -> late + early -> [gate] -> wet low/high cut -> width -> mix.
// Modes set the internal topology (line-length scale, ER pattern/level, diffusion, modulation, damping corner):
//   Hall, Plate (no ER, dense, bright), Room, Chamber, Gated (80s gated reverb: the gate opens when the
//   pre-delayed input exceeds `gate_threshold_db`, holds `gate_hold_ms` after it falls, then closes fast),
//   Shimmer (octave-up pitch shifter fed back into the tank, amount `shimmer`).
// Modal density: 16 lines of 30..93 ms (x size) => ~1 mode/Hz at size 0.5 in Hall.

#include "core/Module.h"
#include "dsp/InterpDelay.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

#include <array>

namespace ks {

class ReverbFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    ReverbFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P {
        Mode, Mix, PredelayMs, Size, Decay, DampHf, DampLf, Early, Diffusion, ModDepth, Width, LowCutHz, HighCutHz,
        GateThresholdDb, GateHoldMs, Shimmer, Count
    };
    enum ModeId { Hall, Plate, Room, Chamber, Gated, ShimmerMode };

    static constexpr int kLines = 16;
    static constexpr int kErTaps = 8;
    static constexpr int kDiffusers = 4;
    static constexpr float kMaxPredelayMs = 250.0f;

    struct ModeShape {
        float lengthScale, erScale, erGain, diffusion, mod, crossoverHz;
    };
    static const ModeShape& shape(int mode) noexcept;

private:
    struct Diffuser {
        dsp::InterpDelay line;
        int length = 1;
        float process(float x, float g) noexcept {
            const float s = line.tap(length - 1);
            const float v = x - g * s;
            line.push(v);
            return s + g * v;
        }
    };
    struct Line {
        dsp::InterpDelay delay;
        dsp::TptOnePole hf, lf;
        float gain = 0.0f, kHf = 1.0f, kLf = 1.0f;
        float oscC = 1.0f, oscS = 0.0f, rotC = 1.0f, rotS = 0.0f; // quadrature LFO
    };
    struct PitchShifter { // octave up, two crossfaded heads (granular)
        dsp::InterpDelay line;
        float phase = 0.0f, window = 2048.0f;
        float process(float x) noexcept;
    };

    void updateLines(float lengthScale, float decay, float dampHf, float dampLf, float crossover) noexcept;

    double sr_ = 48000.0;
    dsp::InterpDelay pre_[2];
    std::array<Diffuser, kDiffusers> diff_[2];
    std::array<Line, kLines> lines_;
    PitchShifter shifter_;
    dsp::TptOnePole wetHp_[2], wetLp_[2];
    dsp::OnePoleSmoother mix_, predelay_, scale_, early_, width_, modDepth_, shimmer_, erScale_, erGain_, diffG_;
    float shimFeed_ = 0.0f;
    // gate
    float gateEnv_ = 0.0f, gateGain_ = 0.0f, gateAtt_ = 0.0f, gateRel_ = 0.0f, envAtt_ = 0.0f, envRel_ = 0.0f;
    int gateHold_ = 0;
    int lastMode_ = -1;
    float lastLow_ = -1.0f, lastHigh_ = -1.0f;
};

} // namespace ks
