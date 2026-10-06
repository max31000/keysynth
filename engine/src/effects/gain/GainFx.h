#pragma once
// `gain`: utility gain + balance pan (smoothed). Zero latency.

#include "core/Module.h"
#include "dsp/Smoother.h"

namespace ks {

class GainFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    GainFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;

    enum P { GainDb, Pan, Count };

private:
    dsp::OnePoleSmoother gl_, gr_;
};

} // namespace ks
