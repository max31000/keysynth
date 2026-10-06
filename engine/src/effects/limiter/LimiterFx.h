#pragma once
// `limiter`: zero-lookahead soft-clip limiter with fast release (dsp::SafetyLimiter). The Engine always applies
// one at the very end as master safety; this module lets presets put one in a chain explicitly.
// Read-only param `gr_db` reports the current gain reduction.

#include "core/Module.h"
#include "dsp/Limiter.h"

namespace ks {

class LimiterFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    LimiterFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;

    enum P { CeilingDb, ReleaseMs, GainReductionDb, Count };

private:
    dsp::SafetyLimiter lim_;
};

} // namespace ks
