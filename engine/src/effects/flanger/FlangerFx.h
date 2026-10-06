#pragma once
// `flanger`: BBD flanger (A/DA Flanger, Electric Mistress, MXR-style). Exponentially swept short delay
// (`time` = manual delay, `depth` = sweep in octaves of delay), regeneration (`feedback`, +/-), stereo spread,
// tempo sync. `through_zero`: tape-style TZF — a fixed reference line of `time` ms replaces the dry path and the
// swept line crosses it, so the comb sweeps through zero delay. NOTE: in that mode the whole output is delayed by
// `time` (an effect choice, max 10 ms, default off); latencySamples() stays 0 as in every built-in module.

#include "core/Module.h"
#include "dsp/Bbd.h"
#include "dsp/Smoother.h"

namespace ks {

class FlangerFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    FlangerFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Time, Depth, Rate, Sync, Feedback, ThroughZero, Spread, Mix, Count };
    static constexpr float kMaxDelayMs = 30.0f;

private:
    struct Channel {
        dsp::Bbd mod, ref;
        dsp::OnePoleSmoother delay;
        float last = 0.0f;
    };
    double sr_ = 48000.0;
    Channel ch_[2];
    dsp::Lfo lfo_;
    dsp::OnePoleSmoother time_, depth_, fb_, spread_, mix_, tz_;
};

} // namespace ks
