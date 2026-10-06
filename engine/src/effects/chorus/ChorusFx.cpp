#include "effects/chorus/ChorusFx.h"

#include "dsp/NoteDivision.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& ChorusFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "chorus";
        i.displayName = "Chorus";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        i.params = {
            enumParam("mode", "Mode", {"I", "II", "I+II", "Custom", "Dimension"}, 0),
            logParam("rate", "Rate", 0.05f, 10.0f, 0.5f, "Hz", {}, 0.8f),
            linearParam("depth", "Depth", 0.0f, 1.0f, 0.5f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.5f),
            linearParam("width", "Width", 0.0f, 1.0f, 1.0f),
            linearParam("hiss", "BBD Hiss", 0.0f, 1.0f, 0.15f),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 0), // replaces `rate` (Custom/Dimension)
        };
        // rate/depth/sync apply to Custom and Dimension; the Juno modes use the original fixed values.
        i.uiHints = {{"front", {"mode", "rate", "depth", "mix"}}};
        return i;
    }();
    return info;
}

ChorusFx::ChorusFx() : Module(moduleInfo()) {}

ChorusFx::Shape ChorusFx::shapeFor(int mode, float rate, float depth) const noexcept {
    switch (mode) {
    case JunoI: return {0.513f, 1.66f, 5.35f, 9000.0f, dsp::Lfo::Shape::Triangle};
    case JunoII: return {0.863f, 1.66f, 5.35f, 9000.0f, dsp::Lfo::Shape::Triangle};
    case JunoI_II: return {9.75f, 3.30f, 3.70f, 9000.0f, dsp::Lfo::Shape::Sine};
    case Dimension: {
        const float sw = 0.2f + 1.6f * depth; // ms either side of 6 ms
        return {rate, 6.0f - sw, 6.0f + sw, 12000.0f, dsp::Lfo::Shape::Triangle};
    }
    default: {
        const float sw = 0.1f + 3.4f * depth; // ms either side of 4 ms
        return {rate, std::max(0.5f, 4.0f - sw), 4.0f + sw, 10000.0f, dsp::Lfo::Shape::Triangle};
    }
    }
}

void ChorusFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    a_.prepare(sampleRate, kMaxDelayMs, 9000.0f);
    b_.prepare(sampleRate, kMaxDelayMs, 9000.0f);
    a_.setNoiseSeed(0x1234567u);
    b_.setNoiseSeed(0x7654321u);
    lfo_.setSampleRate(sampleRate);
    dA_.prepare(sampleRate, 0.002f);
    dB_.prepare(sampleRate, 0.002f);
    mix_.prepare(sampleRate, 0.02f);
    width_.prepare(sampleRate, 0.02f);
    dim_.prepare(sampleRate, 0.03f);
    reset();
}

void ChorusFx::reset() {
    a_.reset();
    b_.reset();
    lfo_.setPhase(0.0f);
    const int mode = static_cast<int>(params().get(Mode));
    const Shape s = shapeFor(mode, params().get(Rate), params().get(Depth));
    const float centre = 0.5f * (s.minMs + s.maxMs) * 0.001f * static_cast<float>(sr_);
    dA_.snap(centre);
    dB_.snap(centre);
    mix_.snap(params().get(Mix));
    width_.snap(params().get(Width));
    dim_.snap(mode == Dimension ? 1.0f : 0.0f);
    lastMode_ = -1;
}

int ChorusFx::tailSamples() const { return static_cast<int>((kMaxDelayMs + dsp::Bbd::kFilterTailMs) * 0.001 * sr_); }

void ChorusFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    const int mode = static_cast<int>(params().get(Mode));
    const double div = dsp::noteDivisionSeconds(static_cast<int>(params().get(Sync)), ctx.transport.tempo);
    // Synced rate stays inside the `rate` range (fast divisions at high tempo would be a ~40 Hz wobble).
    const float rate = div > 0.0 ? std::clamp(static_cast<float>(1.0 / div), 0.05f, 10.0f) : params().get(Rate);
    const Shape s = shapeFor(mode, rate, params().get(Depth));
    if (mode != lastMode_) {
        a_.setCutoff(s.cutoffHz);
        b_.setCutoff(s.cutoffHz);
        lastMode_ = mode;
    }
    const float hiss = params().get(Hiss);
    a_.setHiss(hiss);
    b_.setHiss(hiss);
    lfo_.setRate(s.rateHz);
    mix_.setTarget(params().get(Mix));
    width_.setTarget(params().get(Width));
    dim_.setTarget(mode == Dimension ? 1.0f : 0.0f);

    const float msToS = 0.001f * static_cast<float>(sr_);
    const float centre = 0.5f * (s.minMs + s.maxMs) * msToS;
    const float swing = 0.5f * (s.maxMs - s.minMs) * msToS;

    for (int i = 0; i < io.numSamples; ++i) {
        const float l = io.left[i], r = io.right[i];
        const float mono = 0.5f * (l + r);
        const float m = lfo_.valueAt(s.lfo, 0.0f);
        lfo_.advance();
        dA_.setTarget(centre + swing * m);
        dB_.setTarget(centre - swing * m); // opposite phase
        const float wa = a_.process(mono, dA_.next());
        const float wb = b_.process(mono, dB_.next());
        // Juno: L = A, R = B.  Dimension: cross-subtracted matrix.
        const float d = dim_.next();
        float wl = wa - d * 0.45f * wb;
        float wr = wb - d * 0.45f * wa;
        const float w = width_.next();
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * w;
        wl = mid + side;
        wr = mid - side;
        const float mx = mix_.next();
        io.left[i] = l + (wl - l) * mx;
        io.right[i] = r + (wr - r) * mx;
    }
}

} // namespace ks
