#include "effects/phaser/PhaserFx.h"

#include "dsp/NoteDivision.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& PhaserFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "phaser";
        i.displayName = "Phaser";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        i.params = {
            enumParam("stages", "Stages", {"4", "6", "8", "12"}, 0), // switching re-wires the chain (not smoothed)
            logParam("rate", "Rate", 0.02f, 10.0f, 0.5f, "Hz", {}, 0.5f),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 0),
            linearParam("depth", "Depth", 0.0f, 1.0f, 0.8f),
            logParam("centre_hz", "Centre", 100.0f, 4000.0f, 700.0f, "Hz", {}, 700.0f),
            linearParam("feedback", "Color", -0.95f, 0.95f, 0.3f),
            linearParam("spread", "Stereo Spread", 0.0f, 1.0f, 0.25f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.5f),
        };
        i.params[Stages].flags = ParamFlags::NonAutomatable;
        i.uiHints = {{"front", {"rate", "depth", "feedback", "mix"}}};
        return i;
    }();
    return info;
}

PhaserFx::PhaserFx() : Module(moduleInfo()) {}

int PhaserFx::stageCount(int choice) noexcept {
    static constexpr int counts[] = {4, 6, 8, 12};
    return counts[choice < 0 ? 0 : (choice > 3 ? 3 : choice)];
}

void PhaserFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    lfo_.setSampleRate(sampleRate);
    depth_.prepare(sampleRate, 0.02f);
    centre_.prepare(sampleRate, 0.03f);
    fb_.prepare(sampleRate, 0.02f);
    spread_.prepare(sampleRate, 0.05f);
    mix_.prepare(sampleRate, 0.02f);
    reset();
}

void PhaserFx::reset() {
    for (auto& c : ch_) {
        for (auto& a : c.ap) a.reset();
        c.last = 0.0f;
    }
    lfo_.setPhase(0.0f);
    depth_.snap(params().get(Depth));
    centre_.snap(params().get(CentreHz));
    fb_.snap(params().get(Feedback));
    spread_.snap(params().get(Spread));
    mix_.snap(params().get(Mix));
    lastStages_ = -1;
}

int PhaserFx::tailSamples() const {
    // Each pass through the chain takes ~stages x 1/(pi f) at the lowest swept corner (first-order all-pass
    // group delay at low frequency); feedback decays by |fb| per pass. Count passes to -100 dB.
    const int stages = stageCount(static_cast<int>(params().get(Stages)));
    const float fMin = std::max(params().get(CentreHz) * std::exp2(-2.5f * params().get(Depth)), 10.0f);
    const float pass = static_cast<float>(stages) / (dsp::kPi * fMin);
    const float fb = std::min(std::fabs(params().get(Feedback)), 0.95f);
    const float passes = fb > 1e-3f ? std::log(1e-5f) / std::log(fb) : 1.0f;
    return static_cast<int>(((passes + 2.0f) * pass + 0.05f) * static_cast<float>(sr_));
}

void PhaserFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    const int stages = stageCount(static_cast<int>(params().get(Stages)));
    if (stages != lastStages_) {
        // Stages that were idle hold stale state; clear them so they don't click in.
        for (auto& c : ch_)
            for (int s = lastStages_ < 0 ? 0 : lastStages_; s < kMaxStages; ++s) c.ap[static_cast<size_t>(s)].reset();
        lastStages_ = stages;
    }
    const double div = dsp::noteDivisionSeconds(static_cast<int>(params().get(Sync)), ctx.transport.tempo);
    lfo_.setRate(div > 0.0 ? static_cast<float>(1.0 / div) : params().get(Rate));
    depth_.setTarget(params().get(Depth));
    centre_.setTarget(params().get(CentreHz));
    fb_.setTarget(params().get(Feedback));
    spread_.setTarget(params().get(Spread));
    mix_.setTarget(params().get(Mix));

    const float sr = static_cast<float>(sr_);
    constexpr float kOctaves = 2.5f; // sweep +-2.5 octaves at depth 1
    for (int i = 0; i < io.numSamples; ++i) {
        const float depth = depth_.next(), centre = centre_.next(), fb = fb_.next(), mx = mix_.next();
        const float spreadPhase = 0.5f * spread_.next();
        float* io2[2] = {io.left + i, io.right + i};
        for (int c = 0; c < 2; ++c) {
            const float m = lfo_.valueAt(dsp::Lfo::Shape::Sine, c == 0 ? 0.0f : spreadPhase);
            const float hz = centre * std::exp2(kOctaves * depth * m);
            const float g = dsp::tptG(hz, sr);
            Channel& ch = ch_[c];
            const float x = *io2[c];
            float y = x + fb * ch.last;
            for (int s = 0; s < stages; ++s) {
                auto& ap = ch.ap[static_cast<size_t>(s)];
                ap.setG(g);
                y = ap.ap(y);
            }
            // Soft limit the regeneration path so extreme feedback stays bounded.
            ch.last = y / (1.0f + 0.1f * std::fabs(y));
            *io2[c] = x + (y - x) * mx;
        }
        lfo_.advance();
    }
}

} // namespace ks
