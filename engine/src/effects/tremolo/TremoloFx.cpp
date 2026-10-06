#include "effects/tremolo/TremoloFx.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
constexpr double kDivBeats[] = {4.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
constexpr int kNumDiv = static_cast<int>(sizeof(kDivBeats) / sizeof(kDivBeats[0]));
} // namespace

const ModuleInfo& TremoloFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "tremolo";
        i.displayName = "Tremolo / Autopan";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        i.params = {
            enumParam("mode", "Mode", {"Tremolo", "Autopan"}, 0, "LFO"),
            logParam("rate", "Rate", 0.1f, 20.0f, 5.0f, "Hz", "LFO", 4.0f),
            boolParam("sync", "Tempo Sync", false, "LFO"),
            enumParam("division", "Division",
                      {"1/1", "1/2", "1/2T", "1/4.", "1/4", "1/4T", "1/8.", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 7,
                      "LFO"),
            linearParam("depth", "Depth", 0.0f, 1.0f, 0.5f, {}, "LFO"),
            enumParam("shape", "Shape", {"Sine", "Triangle", "Square"}, 0, "Shape"),
            linearParam("smoothing", "Square Smoothing", 0.0f, 1.0f, 0.5f, {}, "Shape"),
            linearParam("stereo_phase", "Stereo Phase", 0.0f, 180.0f, 0.0f, "deg", "Shape"),
        };
        i.uiHints = {{"groupOrder", {"LFO", "Shape"}}, {"front", {"mode", "rate", "depth", "shape", "stereo_phase"}}};
        return i;
    }();
    return info;
}

TremoloFx::TremoloFx() : Module(moduleInfo()) {}

double TremoloFx::divisionBeats(int index) noexcept { return kDivBeats[std::clamp(index, 0, kNumDiv - 1)]; }

float TremoloFx::lfoValue(double phase, ShapeId shape, float smoothing) noexcept {
    const double ph = phase - std::floor(phase);
    switch (shape) {
    case ShapeId::Triangle: {
        double f = ph + 0.25;
        f -= std::floor(f);
        return static_cast<float>(1.0 - 4.0 * std::fabs(f - 0.5));
    }
    case ShapeId::Square: {
        // tanh-shaped sine: smoothing 1 -> nearly sine, 0 -> hard edges (~0.4 ms at 5 Hz, no clicks).
        const float s = 1.0f - std::clamp(smoothing, 0.0f, 1.0f);
        const float k = 1.5f + 40.0f * s * s;
        return std::tanh(k * static_cast<float>(std::sin(2.0 * std::numbers::pi * ph))) / std::tanh(k);
    }
    default: return static_cast<float>(std::sin(2.0 * std::numbers::pi * ph));
    }
}

void TremoloFx::prepare(double sampleRate, int) {
    sampleRate_ = sampleRate;
    depth_.prepare(sampleRate, 0.02f);
    reset();
}

void TremoloFx::reset() {
    phase_ = 0.0;
    depth_.snap(std::clamp(params().get(Depth), 0.0f, 1.0f));
}

void TremoloFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    const ParamSet& p = params();
    const auto mode = p.get(Mode) >= 0.5f ? ModeId::Autopan : ModeId::Tremolo;
    const auto shape = static_cast<ShapeId>(std::clamp(static_cast<int>(p.get(Shape)), 0, 2));
    const float smoothing = p.get(Smoothing);
    const double stereoOffset = std::clamp(p.get(StereoPhase), 0.0f, 180.0f) / 360.0;
    depth_.setTarget(std::clamp(p.get(Depth), 0.0f, 1.0f));

    double hz = std::clamp(static_cast<double>(p.get(Rate)), 0.1, 20.0);
    double correction = 0.0; // phase error spread over this block (no gain steps on transport jumps)
    if (p.get(Sync) >= 0.5f) {
        const double beats = divisionBeats(static_cast<int>(p.get(Division)));
        const double tempo = std::clamp(ctx.transport.tempo, 20.0, 400.0);
        hz = tempo / 60.0 / beats;
        if (ctx.transport.playing && io.numSamples > 0) { // lock the phase to the bar grid (PLL-style slew)
            const double ph = ctx.transport.ppqPosition / beats;
            double err = (ph - std::floor(ph)) - phase_;
            err -= std::floor(err + 0.5); // wrap to [-0.5, 0.5)
            correction = 0.5 * err / static_cast<double>(io.numSamples);
        }
    }
    const double inc = hz / sampleRate_ + correction;

    for (int i = 0; i < io.numSamples; ++i) {
        const float d = depth_.next();
        if (mode == ModeId::Tremolo) {
            const float sl = lfoValue(phase_, shape, smoothing);
            const float sr = stereoOffset > 0.0 ? lfoValue(phase_ + stereoOffset, shape, smoothing) : sl;
            io.left[i] *= 1.0f - d * 0.5f * (1.0f - sl);
            io.right[i] *= 1.0f - d * 0.5f * (1.0f - sr);
        } else {
            const float pos = d * lfoValue(phase_, shape, smoothing);
            io.left[i] *= std::sqrt(std::max(0.0f, 1.0f - pos));
            io.right[i] *= std::sqrt(std::max(0.0f, 1.0f + pos));
        }
        phase_ += inc;
        phase_ -= std::floor(phase_);
    }
}

} // namespace ks
