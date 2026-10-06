#pragma once
// `ensemble`: string-machine ensemble (ARP Solina / Eminent / Logan style). Three BBD lines fed by the mono sum,
// each modulated by a slow (~0.6 Hz) + fast (~6 Hz) sine LFO pair phased 0/120/240 degrees, so the sum never
// collapses to a single vibrato — the 70s string-synth shimmer. Left = A + B/2, right = C + B/2. Zero latency.
// `tone` sets the BBD filter corner.

#include "core/Module.h"
#include "dsp/Bbd.h"
#include "dsp/Smoother.h"

#include <array>

namespace ks {

class EnsembleFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    EnsembleFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Rate, RateFast, Depth, DepthFast, Mix, Width, Tone, Hiss, Count };

    static constexpr float kCentreMs = 7.0f;
    static constexpr float kMaxDelayMs = 12.0f;

private:
    double sr_ = 48000.0;
    std::array<dsp::Bbd, 3> line_;
    dsp::Lfo slow_, fast_;
    dsp::OnePoleSmoother slowAmt_, fastAmt_, mix_, width_;
    float lastTone_ = -1.0f;
};

} // namespace ks
