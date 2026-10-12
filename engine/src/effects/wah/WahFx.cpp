#include "effects/wah/WahFx.h"

#include "dsp/Math.h"
#include "dsp/NoteDivision.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
struct ModelCurve {
    float heelHz, toeHz;
    float qHeel, qToe; // relative Q at heel / toe (x `q`)
    float body;        // low-passed dry body mixed under the resonance
};
// Cry Baby GCB-95: 350 Hz .. 2.2 kHz, the inductor's Q rises towards the toe. Vox V846: a little higher, flatter.
constexpr ModelCurve kModels[2] = {{350.0f, 2200.0f, 0.8f, 1.25f, 0.18f}, {450.0f, 2600.0f, 0.7f, 0.85f, 0.3f}};
constexpr float kMakeup = 3.0f; // +9.5 dB resonance peak (a real wah peaks well above bypass level)
} // namespace

const ModuleInfo& WahFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "wah";
        i.displayName = "Wah";
        i.kind = ModuleKind::Effect;
        i.category = "Filter";
        i.params = {
            enumParam("mode", "Mode", {"Manual", "Auto", "LFO"}, Manual),
            linearParam("pedal", "Pedal", 0.0f, 1.0f, 0.5f),
            enumParam("pedal_source", "Pedal Source", {"Param", "Mod Wheel", "Expression (CC11)"}, SrcParam),
            enumParam("model", "Model", {"Cry Baby", "Vox"}, CryBaby, "Filter"),
            linearParam("range", "Range", 0.25f, 1.5f, 1.0f, {}, "Filter"),
            logParam("q", "Q", 1.0f, 20.0f, 6.0f, {}, "Filter", 6.0f),
            linearParam("sens", "Sensitivity", 0.0f, 1.0f, 0.5f, {}, "Auto"),
            logParam("attack_ms", "Attack", 0.5f, 100.0f, 4.0f, "ms", "Auto", 5.0f),
            logParam("release_ms", "Release", 10.0f, 2000.0f, 150.0f, "ms", "Auto", 150.0f),
            logParam("rate", "Rate", 0.05f, 12.0f, 1.0f, "Hz", "LFO", 1.0f),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 0, "LFO"),
            linearParam("depth", "Depth", 0.0f, 1.0f, 1.0f, {}, "LFO"),
            linearParam("level_db", "Level", -24.0f, 12.0f, 0.0f, "dB"),
            linearParam("mix", "Mix", 0.0f, 1.0f, 1.0f),
        };
        i.uiHints = {{"groupOrder", {"", "Filter", "Auto", "LFO"}},
                     {"front", {"mode", "pedal", "pedal_source", "model", "q", "sens", "mix"}}};
        return i;
    }();
    return info;
}

WahFx::WahFx() : Module(moduleInfo()) {}

void WahFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    for (auto& s : svf_) s.setSampleRate(sampleRate);
    for (auto& b : body_) b.setCutoff(700.0f, static_cast<float>(sampleRate));
    pos_.prepare(sampleRate, 0.006f);
    mix_.prepare(sampleRate, 0.02f);
    level_.prepare(sampleRate, 0.02f);
    reset();
}

void WahFx::reset() {
    for (auto& s : svf_) s.reset();
    for (auto& b : body_) b.reset();
    env_.reset();
    phase_ = 0.0;
    pos_.snap(std::clamp(params().get(Pedal), 0.0f, 1.0f));
    mix_.snap(params().get(Mix));
    level_.snap(dsp::dbToGain(params().get(LevelDb)) * kMakeup);
}

int WahFx::tailSamples() const {
    // Band-pass ring: Q / (pi f) to -60 dB is ~ 6.9 Q / (pi f); worst case Q 20 x 1.25 at 200 Hz heel -> ~0.3 s.
    return static_cast<int>(0.3 * sr_);
}

float WahFx::centreHz(float position) const noexcept {
    const ModelCurve& m = kModels[std::clamp(static_cast<int>(params().get(Model)), 0, 1)];
    const float range = std::clamp(params().get(Range), 0.25f, 1.5f);
    const float centre = std::sqrt(m.heelHz * m.toeHz);
    const float halfOct = 0.5f * std::log2(m.toeHz / m.heelHz) * range;
    const float p = std::clamp(position, 0.0f, 1.0f);
    return centre * std::exp2(halfOct * (2.0f * p - 1.0f));
}

void WahFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    const ParamSet& p = params();
    const int mode = std::clamp(static_cast<int>(p.get(Mode)), 0, 2);
    const ModelCurve& m = kModels[std::clamp(static_cast<int>(p.get(Model)), 0, 1)];
    const float q = std::clamp(p.get(Q), 1.0f, 20.0f);
    const float sr = static_cast<float>(sr_);
    float base = std::clamp(p.get(Pedal), 0.0f, 1.0f);
    if (mode == Manual && ctx.channel != nullptr) {
        const int src = static_cast<int>(p.get(PedalSource));
        if (src == SrcModWheel) base = std::clamp(ctx.channel->modWheel, 0.0f, 1.0f);
        else if (src == SrcExpression) base = std::clamp(ctx.channel->expression, 0.0f, 1.0f);
    }
    const float sens = p.get(Sens);
    env_.setTimes(p.get(AttackMs) * 0.001f, p.get(ReleaseMs) * 0.001f, sr_);
    const float depth = std::clamp(p.get(Depth), 0.0f, 1.0f);
    mix_.setTarget(p.get(Mix));
    level_.setTarget(dsp::dbToGain(p.get(LevelDb)) * kMakeup);

    // LFO (same transport lock as `tremolo`: free `rate`, or `sync` with a PLL slew to the bar grid).
    double hz = std::clamp(static_cast<double>(p.get(Rate)), 0.05, 12.0);
    double correction = 0.0;
    if (const double beats = dsp::noteDivisionBeats(static_cast<int>(p.get(Sync))); beats > 0.0) {
        hz = std::clamp(ctx.transport.tempo, 20.0, 400.0) / 60.0 / beats;
        if (ctx.transport.playing && io.numSamples > 0) {
            const double ph = ctx.transport.ppqPosition / beats;
            double err = (ph - std::floor(ph)) - phase_;
            err -= std::floor(err + 0.5);
            correction = 0.5 * err / static_cast<double>(io.numSamples);
        }
    }
    const double inc = hz / sr_ + correction;

    // Log-frequency mapping constants (centreHz() inlined).
    const float range = std::clamp(p.get(Range), 0.25f, 1.5f);
    const float centre = std::sqrt(m.heelHz * m.toeHz);
    const float halfOct = 0.5f * std::log2(m.toeHz / m.heelHz) * range;
    const float nyq = 0.45f * sr;

    for (int i = 0; i < io.numSamples; ++i) {
        const float xl = io.left[i], xr = io.right[i];
        float target = base;
        if (mode == Auto) {
            const float e = env_.process(0.5f * (xl + xr));
            target = base + sens * std::fmin(3.0f * e, 1.0f) * (1.0f - base); // louder -> towards the toe
        } else if (mode == LfoMode) {
            const float u = 0.5f - 0.5f * std::cos(dsp::kTwoPi * static_cast<float>(phase_));
            target = base + (u - base) * depth;
            phase_ += inc;
            phase_ -= std::floor(phase_);
        }
        pos_.setTarget(std::clamp(target, 0.0f, 1.0f));
        const float pos = pos_.next();
        const float fc = std::fmin(centre * std::exp2(halfOct * (2.0f * pos - 1.0f)), nyq);
        const float qq = q * (m.qHeel + (m.qToe - m.qHeel) * pos);
        const float k = 1.0f / qq;
        const float g = std::tan(dsp::kPi * fc / sr);
        const float mx = mix_.next(), lv = level_.next();
        float x[2] = {xl, xr};
        for (int c = 0; c < 2; ++c) {
            svf_[c].setGK(g, k);
            const dsp::Svf::Out o = svf_[c].tick(x[c]);
            const float wet = (k * o.bp + m.body * body_[c].lp(x[c])) * lv;
            x[c] = x[c] + (wet - x[c]) * mx;
        }
        io.left[i] = x[0];
        io.right[i] = x[1];
    }
}

} // namespace ks
