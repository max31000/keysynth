#include "effects/gain/GainFx.h"

#include "dsp/Math.h"

namespace ks {

const ModuleInfo& GainFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "gain";
        i.displayName = "Gain";
        i.kind = ModuleKind::Effect;
        i.category = "Utility";
        i.params = {
            linearParam("gain_db", "Gain", -60.0f, 24.0f, 0.0f, "dB"),
            linearParam("pan", "Pan", -1.0f, 1.0f, 0.0f),
        };
        return i;
    }();
    return info;
}

GainFx::GainFx() : Module(moduleInfo()) {}

void GainFx::prepare(double sampleRate, int) {
    gl_.prepare(sampleRate, 0.01f);
    gr_.prepare(sampleRate, 0.01f);
    reset();
}

void GainFx::reset() {
    const float g = dsp::dbToGain(params().get(GainDb));
    const float p = params().get(Pan);
    gl_.snap(g * (p <= 0.0f ? 1.0f : 1.0f - p));
    gr_.snap(g * (p >= 0.0f ? 1.0f : 1.0f + p));
}

void GainFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    const float g = dsp::dbToGain(params().get(GainDb));
    const float p = params().get(Pan);
    gl_.setTarget(g * (p <= 0.0f ? 1.0f : 1.0f - p));
    gr_.setTarget(g * (p >= 0.0f ? 1.0f : 1.0f + p));
    for (int i = 0; i < io.numSamples; ++i) {
        io.left[i] *= gl_.next();
        io.right[i] *= gr_.next();
    }
}

} // namespace ks
