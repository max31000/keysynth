#include "effects/echo/EchoFx.h"

#include "dsp/Math.h"
#include "dsp/NoteDivision.h"
#include "dsp/SoftClip.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
// Selector positions (labels list the heads that play). Order is a preset-format surface.
constexpr unsigned kCombos[EchoFx::kNumCombos] = {0b0001, 0b0010, 0b0100, 0b1000, 0b0011, 0b0110,
                                                  0b1100, 0b0101, 0b1010, 0b0111, 0b1110, 0b1111};
// Slight playback-head differences (gap, azimuth): later heads a touch quieter.
constexpr float kHeadLevel[4] = {1.0f, 0.94f, 0.9f, 0.86f};
constexpr float kSwellMax = 1.08f; // loop gain at feedback 1: just above unity -> bounded runaway (tanh)
} // namespace

const ModuleInfo& EchoFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "echo";
        i.displayName = "Echo (Drum)";
        i.kind = ModuleKind::Effect;
        i.category = "Delay";
        i.params = {
            enumParam("heads", "Heads",
                      {"1", "2", "3", "4", "1+2", "2+3", "3+4", "1+3", "2+4", "1+2+3", "2+3+4", "1+2+3+4"}, 11),
            logParam("time", "Head Spacing", 20.0f, 1000.0f, 110.0f, "ms", {}, 150.0f),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 0),
            linearParam("feedback", "Swell", 0.0f, 1.0f, 0.35f),
            linearParam("tone", "Tone", 0.0f, 1.0f, 0.5f),
            linearParam("drive", "Saturation", 0.0f, 1.0f, 0.3f, {}, "Drum"),
            linearParam("wow", "Wow", 0.0f, 1.0f, 0.25f, {}, "Drum"),
            linearParam("flutter", "Flutter", 0.0f, 1.0f, 0.25f, {}, "Drum"),
            linearParam("width", "Width", 0.0f, 1.0f, 0.5f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.35f),
        };
        i.uiHints = {{"front", {"heads", "time", "sync", "feedback", "tone", "mix"}}};
        return i;
    }();
    return info;
}

EchoFx::EchoFx() : Module(moduleInfo()) {}

unsigned EchoFx::headMask(int headsIndex) noexcept { return kCombos[std::clamp(headsIndex, 0, kNumCombos - 1)]; }

float EchoFx::spacingSamples(double tempo) const noexcept {
    const double div = dsp::noteDivisionSeconds(static_cast<int>(params().get(Sync)), tempo);
    const double sec = div > 0.0 ? div : static_cast<double>(params().get(Time)) * 0.001;
    return static_cast<float>(std::clamp(sec, 0.005, kMaxSpacingSeconds) * sr_);
}

void EchoFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    // 4 heads x max spacing + max wow excursion + interpolation guard.
    drum_.prepare(static_cast<int>(4.0 * kMaxSpacingSeconds * 1.02 * sampleRate) + 16);
    loopHp_.setCutoff(90.0f, static_cast<float>(sampleRate));
    wow_.setSampleRate(sampleRate);
    flutter_.setSampleRate(sampleRate);
    wow_.setRate(0.7f);
    flutter_.setRate(9.5f);
    spacing_.prepare(sampleRate, 0.15f); // speed changes glide like the drum motor
    fb_.prepare(sampleRate, 0.03f);
    mix_.prepare(sampleRate, 0.02f);
    width_.prepare(sampleRate, 0.02f);
    wowAmt_.prepare(sampleRate, 0.05f);
    flutAmt_.prepare(sampleRate, 0.05f);
    drive_.prepare(sampleRate, 0.05f);
    for (auto& g : headGain_) g.prepare(sampleRate, 0.01f);
    reset();
}

void EchoFx::reset() {
    drum_.reset();
    loopHp_.reset();
    loopLp_.reset();
    wow_.setPhase(0.0f);
    flutter_.setPhase(0.37f);
    spacing_.snap(spacingSamples(lastTempo_.load(std::memory_order_relaxed)));
    fb_.snap(params().get(Feedback));
    mix_.snap(params().get(Mix));
    width_.snap(params().get(Width));
    wowAmt_.snap(params().get(Wow));
    flutAmt_.snap(params().get(Flutter));
    drive_.snap(params().get(Drive));
    const unsigned mask = headMask(static_cast<int>(params().get(Heads)));
    for (int k = 0; k < 4; ++k) headGain_[static_cast<size_t>(k)].snap((mask >> k) & 1u ? 1.0f : 0.0f);
    lastTone_ = -1.0f;
}

int EchoFx::tailSamples() const {
    const unsigned mask = headMask(static_cast<int>(params().get(Heads)));
    int last = 1;
    for (int k = 0; k < 4; ++k)
        if ((mask >> k) & 1u) last = k + 1;
    const double d = static_cast<double>(spacingSamples(lastTempo_.load(std::memory_order_relaxed))) * last * 1.01;
    const float g = std::min(params().get(Feedback), 1.0f) * kSwellMax;
    // Repeats until -90 dB through the strongest path (loop filters ignored -> conservative); runaway capped at 120 s.
    // The shortest selected head repeats fastest, the longest one ends last: passes x longest head.
    const double passes = g >= 0.999f ? 1e9 : (g > 1e-4f ? std::log(3.16e-5) / std::log(static_cast<double>(g)) : 0.0);
    const double s = std::min((passes + 1.0) * d, 120.0 * sr_);
    return static_cast<int>(s + 0.05 * sr_);
}

void EchoFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    lastTempo_.store(ctx.transport.tempo, std::memory_order_relaxed);
    const ParamSet& p = params();
    const float sr = static_cast<float>(sr_);
    if (const float tone = p.get(Tone); tone != lastTone_) {
        // tone 0..1 -> loop low-pass 1.2 kHz .. 12 kHz (Echorec repeats are mid-focused and darken each pass).
        loopLp_.set(dsp::TptSvf::Type::LowPass, 1200.0f * std::exp2(tone * 3.32f), 0.6f, 0.0f, sr);
        lastTone_ = tone;
    }
    spacing_.setTarget(spacingSamples(ctx.transport.tempo));
    fb_.setTarget(p.get(Feedback));
    mix_.setTarget(p.get(Mix));
    width_.setTarget(p.get(Width));
    wowAmt_.setTarget(p.get(Wow));
    flutAmt_.setTarget(p.get(Flutter));
    drive_.setTarget(p.get(Drive));
    const unsigned mask = headMask(static_cast<int>(p.get(Heads)));
    int nSel = 0;
    for (int k = 0; k < 4; ++k) {
        const bool on = (mask >> k) & 1u;
        headGain_[static_cast<size_t>(k)].setTarget(on ? 1.0f : 0.0f);
        nSel += on ? 1 : 0;
    }
    const float loopNorm = kSwellMax / static_cast<float>(std::max(nSel, 1));
    const float outNorm = 0.85f / std::sqrt(static_cast<float>(std::max(nSel, 1)));
    const float maxD = drum_.maxDelay() - 2.0f;
    // Wow/flutter excursion of head k: speedErr x k x min(spacing, 100 ms). A drum's delay error grows with the
    // head distance; capped at Echorec-like spacings so long synced spacings do not wobble by semitones.
    const float modRef = std::min(spacing_.target(), 0.1f * sr);

    for (int i = 0; i < io.numSamples; ++i) {
        const float xl = io.left[i], xr = io.right[i];
        const float sp = spacing_.next();
        const float fb = fb_.next(), mx = mix_.next(), w = width_.next();
        const float wa = wowAmt_.next(), fa = flutAmt_.next(), drv = drive_.next();
        // Relative drum speed error: wow +-0.35 %, flutter +-0.06 % at 1.
        const float speedErr = wa * 0.0035f * wow_.valueAt(dsp::Lfo::Shape::Sine, 0.0f) +
                               fa * 0.0006f * flutter_.valueAt(dsp::Lfo::Shape::Sine, 0.0f);
        wow_.advance();
        flutter_.advance();

        float sumAll = 0.0f, outL = 0.0f, outR = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float g = headGain_[static_cast<size_t>(k)].next();
            if (g <= 0.0f) continue;
            // Read before writing this sample: a delay of T samples == read(T - 1).
            const float kk = static_cast<float>(k + 1);
            const float d = std::clamp(sp * kk + modRef * kk * speedErr - 1.0f, 1.0f, maxD);
            const float h = drum_.readHermite(d) * kHeadLevel[k] * g;
            sumAll += h;
            const float pan = (k % 2 == 0 ? -0.7f : 0.7f) * w;
            float gl = 0.0f, gr = 0.0f;
            dsp::panGains(pan, gl, gr);
            outL += h * gl * 1.41421356f;
            outR += h * gr * 1.41421356f;
        }
        // Record: input + regeneration through the loop filters and the valve record amp (soft, unity small-signal).
        float rec = 0.5f * (xl + xr) + fb * loopNorm * sumAll;
        rec = loopLp_.process(loopHp_.hp(rec));
        const float kd = 0.6f + 2.4f * drv;
        rec = std::tanh(rec * kd) / kd;
        rec = dsp::softClipKnee(rec, 1.0f, 1.5f);
        drum_.push(rec);

        const float wl = outL * outNorm, wr = outR * outNorm;
        io.left[i] = xl + (wl - xl) * mx;
        io.right[i] = xr + (wr - xr) * mx;
    }
}

} // namespace ks
