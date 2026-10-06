#include "effects/flanger/FlangerFx.h"

#include "dsp/NoteDivision.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& FlangerFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "flanger";
        i.displayName = "Flanger";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        i.params = {
            logParam("time", "Manual", 0.1f, 10.0f, 2.0f, "ms", {}, 1.5f),
            linearParam("depth", "Depth", 0.0f, 1.0f, 0.7f),
            logParam("rate", "Rate", 0.02f, 10.0f, 0.25f, "Hz", {}, 0.4f),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 0),
            linearParam("feedback", "Feedback", -0.98f, 0.98f, 0.5f),
            boolParam("through_zero", "Through Zero", false),
            linearParam("spread", "Stereo Spread", 0.0f, 1.0f, 0.25f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.5f),
        };
        i.uiHints = {{"front", {"time", "depth", "rate", "feedback", "mix"}}};
        return i;
    }();
    return info;
}

FlangerFx::FlangerFx() : Module(moduleInfo()) {}

void FlangerFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    uint32_t seed = 0xF1A9u;
    for (auto& c : ch_) {
        c.mod.prepare(sampleRate, kMaxDelayMs, 10000.0f);
        c.ref.prepare(sampleRate, kMaxDelayMs, 10000.0f);
        c.mod.setHiss(0.05f);
        c.mod.setNoiseSeed(seed++);
        c.delay.prepare(sampleRate, 0.001f);
    }
    lfo_.setSampleRate(sampleRate);
    time_.prepare(sampleRate, 0.05f);
    depth_.prepare(sampleRate, 0.02f);
    fb_.prepare(sampleRate, 0.02f);
    spread_.prepare(sampleRate, 0.05f);
    mix_.prepare(sampleRate, 0.02f);
    tz_.prepare(sampleRate, 0.01f);
    reset();
}

void FlangerFx::reset() {
    const float t = params().get(Time) * 0.001f * static_cast<float>(sr_);
    for (auto& c : ch_) {
        c.mod.reset();
        c.ref.reset();
        c.delay.snap(t);
        c.last = 0.0f;
    }
    lfo_.setPhase(0.0f);
    time_.snap(params().get(Time));
    depth_.snap(params().get(Depth));
    fb_.snap(params().get(Feedback));
    spread_.snap(params().get(Spread));
    mix_.snap(params().get(Mix));
    tz_.snap(params().get(ThroughZero) > 0.5f ? 1.0f : 0.0f);
}

int FlangerFx::tailSamples() const {
    const float fb = std::min(std::fabs(params().get(Feedback)), 0.98f);
    // Passes until -100 dB, each pass at most the longest swept delay.
    const float passes = fb > 1e-3f ? std::log(1e-5f) / std::log(fb) : 1.0f;
    const float maxMs = std::min(params().get(Time) * std::exp2(2.5f * params().get(Depth)), kMaxDelayMs);
    return static_cast<int>(((passes + 1.0f) * maxMs + dsp::Bbd::kFilterTailMs) * 0.001f * static_cast<float>(sr_));
}

void FlangerFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    const double div = dsp::noteDivisionSeconds(static_cast<int>(params().get(Sync)), ctx.transport.tempo);
    lfo_.setRate(div > 0.0 ? static_cast<float>(1.0 / div) : params().get(Rate));
    time_.setTarget(params().get(Time));
    depth_.setTarget(params().get(Depth));
    fb_.setTarget(params().get(Feedback));
    spread_.setTarget(params().get(Spread));
    mix_.setTarget(params().get(Mix));
    tz_.setTarget(params().get(ThroughZero) > 0.5f ? 1.0f : 0.0f);

    const float msToS = 0.001f * static_cast<float>(sr_);
    const float maxD = (kMaxDelayMs - 1.0f) * msToS;
    for (int i = 0; i < io.numSamples; ++i) {
        const float tms = time_.next(), depth = depth_.next(), fb = fb_.next(), mx = mix_.next(), tz = tz_.next();
        const float spreadPhase = 0.5f * spread_.next();
        const float t = tms * msToS;
        float* p[2] = {io.left + i, io.right + i};
        for (int c = 0; c < 2; ++c) {
            Channel& ch = ch_[c];
            const float m = lfo_.valueAt(dsp::Lfo::Shape::Triangle, c == 0 ? 0.0f : spreadPhase);
            // Normal: exponential sweep around `time`. Through-zero: linear sweep 0 .. 2*time around the reference.
            const float dNormal = t * std::exp2(2.5f * depth * m);
            const float dTz = t * (1.0f + depth * m);
            ch.delay.setTarget(std::clamp(dNormal + (dTz - dNormal) * tz, 1.0f, maxD));
            const float x = *p[c];
            const float in = x + fb * ch.last;
            const float wet = ch.mod.process(in, ch.delay.next());
            ch.last = wet / (1.0f + 0.15f * std::fabs(wet)); // bounded regeneration
            // Dry path: the input, or (through-zero) the matched-filter reference line at `time`.
            // The reference always runs so it holds current audio when through-zero is switched on.
            const float ref = ch.ref.process(x, std::max(t, 1.0f));
            const float dry = x + (ref - x) * tz;
            *p[c] = dry + (wet - dry) * mx;
        }
        lfo_.advance();
    }
}

} // namespace ks
