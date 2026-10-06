#include "effects/limiter/LimiterFx.h"

namespace ks {

const ModuleInfo& LimiterFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "limiter";
        i.displayName = "Limiter";
        i.kind = ModuleKind::Effect;
        i.category = "Dynamics";
        auto gr = linearParam("gr_db", "Gain Reduction", 0.0f, 60.0f, 0.0f, "dB");
        gr.flags = ParamFlags::ReadOnly;
        i.params = {
            linearParam("ceiling_db", "Ceiling", -24.0f, 0.0f, -0.3f, "dB"),
            logParam("release_ms", "Release", 5.0f, 1000.0f, 60.0f, "ms", {}, 60.0f),
            gr,
        };
        return i;
    }();
    return info;
}

LimiterFx::LimiterFx() : Module(moduleInfo()) {}

void LimiterFx::prepare(double sampleRate, int) {
    lim_.setCeilingDb(params().get(CeilingDb));
    lim_.setRelease(params().get(ReleaseMs));
    lim_.prepare(sampleRate);
}

void LimiterFx::reset() { lim_.reset(); }

void LimiterFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    lim_.setCeilingDb(params().get(CeilingDb));
    lim_.setRelease(params().get(ReleaseMs));
    lim_.process(io.left, io.right, io.numSamples);
    params().setRaw(GainReductionDb, lim_.gainReductionDb());
}

} // namespace ks
