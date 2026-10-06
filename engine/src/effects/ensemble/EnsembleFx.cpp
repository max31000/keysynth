#include "effects/ensemble/EnsembleFx.h"

#include <cmath>

namespace ks {

const ModuleInfo& EnsembleFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "ensemble";
        i.displayName = "Ensemble";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        i.params = {
            logParam("rate", "Slow Rate", 0.1f, 3.0f, 0.6f, "Hz", "LFO", 0.6f),
            logParam("rate_fast", "Fast Rate", 2.0f, 12.0f, 6.0f, "Hz", "LFO", 6.0f),
            linearParam("depth", "Slow Depth", 0.0f, 1.0f, 0.6f, {}, "LFO"),
            linearParam("depth_fast", "Fast Depth", 0.0f, 1.0f, 0.4f, {}, "LFO"),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.8f),
            linearParam("width", "Width", 0.0f, 1.0f, 1.0f),
            linearParam("tone", "Tone", 0.0f, 1.0f, 0.6f),
            linearParam("hiss", "BBD Hiss", 0.0f, 1.0f, 0.1f),
        };
        i.uiHints = {{"front", {"depth", "depth_fast", "mix", "tone"}}};
        return i;
    }();
    return info;
}

EnsembleFx::EnsembleFx() : Module(moduleInfo()) {}

void EnsembleFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    uint32_t seed = 0xA5A5A5u;
    for (auto& l : line_) {
        l.prepare(sampleRate, kMaxDelayMs, 8000.0f);
        l.setNoiseSeed(seed);
        seed = seed * 1664525u + 1013904223u;
    }
    slow_.setSampleRate(sampleRate);
    fast_.setSampleRate(sampleRate);
    slowAmt_.prepare(sampleRate, 0.03f);
    fastAmt_.prepare(sampleRate, 0.03f);
    mix_.prepare(sampleRate, 0.02f);
    width_.prepare(sampleRate, 0.02f);
    reset();
}

void EnsembleFx::reset() {
    for (auto& l : line_) l.reset();
    slow_.setPhase(0.0f);
    fast_.setPhase(0.0f);
    slowAmt_.snap(params().get(Depth));
    fastAmt_.snap(params().get(DepthFast));
    mix_.snap(params().get(Mix));
    width_.snap(params().get(Width));
    lastTone_ = -1.0f;
}

int EnsembleFx::tailSamples() const { return static_cast<int>((kMaxDelayMs + dsp::Bbd::kFilterTailMs) * 0.001 * sr_); }

void EnsembleFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    const float tone = params().get(Tone);
    if (tone != lastTone_) {
        const float hz = 3000.0f * std::pow(5.0f, tone); // 3 .. 15 kHz
        for (auto& l : line_) l.setCutoff(hz);
        lastTone_ = tone;
    }
    const float hiss = params().get(Hiss);
    for (auto& l : line_) l.setHiss(hiss);
    slow_.setRate(params().get(Rate));
    fast_.setRate(params().get(RateFast));
    slowAmt_.setTarget(params().get(Depth));
    fastAmt_.setTarget(params().get(DepthFast));
    mix_.setTarget(params().get(Mix));
    width_.setTarget(params().get(Width));

    const float msToS = 0.001f * static_cast<float>(sr_);
    const float centre = kCentreMs * msToS;
    const float slowSwing = 3.0f * msToS; // at depth 1: +-3 ms
    const float fastSwing = 0.45f * msToS; // at depth_fast 1: +-0.45 ms
    constexpr float kThird = 1.0f / 3.0f;

    for (int i = 0; i < io.numSamples; ++i) {
        const float l = io.left[i], r = io.right[i];
        const float mono = 0.5f * (l + r);
        const float sa = slowAmt_.next() * slowSwing, fa = fastAmt_.next() * fastSwing;
        float v[3];
        for (int k = 0; k < 3; ++k) {
            const float off = kThird * static_cast<float>(k);
            const float d = centre + sa * slow_.valueAt(dsp::Lfo::Shape::Sine, off) +
                            fa * fast_.valueAt(dsp::Lfo::Shape::Sine, off);
            v[k] = line_[static_cast<size_t>(k)].process(mono, d);
        }
        slow_.advance();
        fast_.advance();
        float wl = 0.67f * (v[0] + 0.5f * v[1]);
        float wr = 0.67f * (v[2] + 0.5f * v[1]);
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
