#pragma once
// `chorus`: Roland Juno-60/106 BBD chorus + a Dimension-style mode. Zero latency.
//   Modes I / II / I+II use the Juno's fixed LFO rate and delay sweep (triangle LFO, 1.66..5.35 ms; I+II: fast
//   9.75 Hz shallow sweep). The two BBD lines get the mono sum and opposite-phase LFOs; left = line A, right =
//   line B (the real unit's stereo). `Custom` uses `rate`/`depth`; `Dimension` is a Dimension-D-like 2-line
//   matrix with cross-subtracted outputs (subtle, wide, little pitch wobble), also on `rate`/`depth`.
// BBD character: dsp::Bbd (anti-alias/reconstruction filters, mild nonlinearity, compander hiss `hiss`).

#include "core/Module.h"
#include "dsp/Bbd.h"
#include "dsp/Smoother.h"

namespace ks {

class ChorusFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    ChorusFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Mode, Rate, Depth, Mix, Width, Hiss, Count };
    enum ModeId { JunoI, JunoII, JunoI_II, Custom, Dimension };

    static constexpr float kMaxDelayMs = 16.0f;

private:
    struct Shape {
        float rateHz, minMs, maxMs, cutoffHz;
        dsp::Lfo::Shape lfo;
    };
    Shape shapeFor(int mode, float rate, float depth) const noexcept;

    double sr_ = 48000.0;
    dsp::Bbd a_, b_;
    dsp::Lfo lfo_;
    // Delay smoothing (2 ms) rounds the triangle corners like the Juno's LFO filter and makes mode switches glide.
    dsp::OnePoleSmoother dA_, dB_, mix_, width_, dim_;
    int lastMode_ = -1;
};

} // namespace ks
