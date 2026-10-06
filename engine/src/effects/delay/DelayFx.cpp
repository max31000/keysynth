#include "effects/delay/DelayFx.h"

#include "dsp/NoteDivision.h"
#include "dsp/SoftClip.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& DelayFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "delay";
        i.displayName = "Delay";
        i.kind = ModuleKind::Effect;
        i.category = "Delay";
        i.params = {
            enumParam("mode", "Mode", {"Stereo", "Ping-Pong", "Tape"}, 0),
            logParam("time", "Time", 1.0f, 2000.0f, 375.0f, "ms", {}, 300.0f),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 0),
            linearParam("feedback", "Feedback", 0.0f, 1.0f, 0.35f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.3f),
            logParam("low_cut_hz", "Low Cut", 20.0f, 2000.0f, 80.0f, "Hz", "Filter", 200.0f),
            logParam("high_cut_hz", "High Cut", 1000.0f, 20000.0f, 9000.0f, "Hz", "Filter", 5000.0f),
            linearParam("offset", "R Offset", -50.0f, 50.0f, 0.0f, "%", "Stereo"),
            linearParam("width", "Width", 0.0f, 1.0f, 1.0f, {}, "Stereo"),
            linearParam("duck", "Ducking", 0.0f, 1.0f, 0.0f),
            linearParam("wow", "Wow", 0.0f, 1.0f, 0.3f, {}, "Tape"),
            linearParam("flutter", "Flutter", 0.0f, 1.0f, 0.3f, {}, "Tape"),
            linearParam("drive", "Saturation", 0.0f, 1.0f, 0.3f, {}, "Tape"),
        };
        i.uiHints = {{"front", {"time", "sync", "feedback", "mix"}}};
        return i;
    }();
    return info;
}

DelayFx::DelayFx() : Module(moduleInfo()) {}

void DelayFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    for (auto& c : ch_) {
        c.line.prepare(static_cast<int>(kMaxSeconds * sampleRate) + 8);
        c.delay.prepare(sampleRate, 0.12f);
    }
    wow_.setSampleRate(sampleRate);
    flutter_.setSampleRate(sampleRate);
    wow_.setRate(0.55f);
    flutter_.setRate(6.8f);
    fb_.prepare(sampleRate, 0.02f);
    mix_.prepare(sampleRate, 0.02f);
    width_.prepare(sampleRate, 0.02f);
    duck_.prepare(sampleRate, 0.02f);
    wowAmt_.prepare(sampleRate, 0.05f);
    flutAmt_.prepare(sampleRate, 0.05f);
    drive_.prepare(sampleRate, 0.05f);
    envAtt_ = dsp::onePoleCoef(0.005f, sampleRate);
    envRel_ = dsp::onePoleCoef(0.25f, sampleRate);
    reset();
}

float DelayFx::targetDelaySamples(const ProcessContext& ctx) const noexcept {
    const double div = dsp::noteDivisionSeconds(static_cast<int>(params().get(Sync)), ctx.transport.tempo);
    const double sec = div > 0.0 ? div : static_cast<double>(params().get(Time)) * 0.001;
    return static_cast<float>(std::clamp(sec, 0.001, kMaxSeconds * 0.98) * sr_);
}

void DelayFx::reset() {
    ProcessContext ctx;
    ctx.transport.tempo = lastTempo_.load(std::memory_order_relaxed);
    const float d = targetDelaySamples(ctx);
    const float off = 1.0f + params().get(Offset) * 0.01f;
    ch_[0].delay.snap(d);
    ch_[1].delay.snap(std::clamp(d * off, 2.0f, static_cast<float>(kMaxSeconds * 0.98 * sr_)));
    for (auto& c : ch_) {
        c.line.reset();
        c.hp.reset();
        c.lp.reset();
    }
    wow_.setPhase(0.0f);
    flutter_.setPhase(0.3f);
    fb_.snap(params().get(Feedback));
    mix_.snap(params().get(Mix));
    width_.snap(params().get(Width));
    duck_.snap(params().get(Duck));
    const bool tape = static_cast<int>(params().get(Mode)) == Tape;
    wowAmt_.snap(tape ? params().get(Wow) : 0.0f);
    flutAmt_.snap(tape ? params().get(Flutter) : 0.0f);
    drive_.snap(tape ? params().get(Drive) : 0.0f);
    env_ = 0.0f;
    lastLow_ = lastHigh_ = -1.0f;
}

int DelayFx::tailSamples() const {
    ProcessContext ctx;
    ctx.transport.tempo = lastTempo_.load(std::memory_order_relaxed);
    const float off = std::max(1.0f, 1.0f + params().get(Offset) * 0.01f);
    const float d = targetDelaySamples(ctx) * off;
    const float g = std::min(params().get(Feedback), 1.0f);
    // Repeats until -90 dB (loop filters ignored -> conservative); infinite feedback capped at 120 s.
    const float passes = g >= 0.999f ? 1e9f : (g > 1e-4f ? std::log(3.16e-5f) / std::log(g) : 0.0f);
    const double s = std::min(static_cast<double>(passes + 1.0f) * d, 120.0 * sr_);
    return static_cast<int>(s + 0.05 * sr_);
}

void DelayFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    lastTempo_.store(ctx.transport.tempo, std::memory_order_relaxed);
    const int mode = static_cast<int>(params().get(Mode));
    const bool tape = mode == Tape;
    const float sr = static_cast<float>(sr_);
    const float low = params().get(LowCutHz), high = params().get(HighCutHz);
    if (low != lastLow_ || high != lastHigh_) {
        for (auto& c : ch_) {
            c.hp.setCutoff(low, sr);
            c.lp.set(dsp::TptSvf::Type::LowPass, high, 0.7071f, 0.0f, sr);
        }
        lastLow_ = low;
        lastHigh_ = high;
    }
    const float d = targetDelaySamples(ctx);
    const float maxD = static_cast<float>(kMaxSeconds * 0.98 * sr_);
    ch_[0].delay.setTarget(d);
    ch_[1].delay.setTarget(std::clamp(d * (1.0f + params().get(Offset) * 0.01f), 2.0f, maxD));
    fb_.setTarget(params().get(Feedback));
    mix_.setTarget(params().get(Mix));
    width_.setTarget(params().get(Width));
    duck_.setTarget(params().get(Duck));
    wowAmt_.setTarget(tape ? params().get(Wow) : 0.0f);
    flutAmt_.setTarget(tape ? params().get(Flutter) : 0.0f);
    drive_.setTarget(tape ? params().get(Drive) : 0.0f);

    const float wowSamples = 0.0025f * sr;  // +-2.5 ms at wow 1
    const float flutSamples = 0.00012f * sr; // +-0.12 ms at flutter 1

    for (int i = 0; i < io.numSamples; ++i) {
        const float xl = io.left[i], xr = io.right[i];
        const float fb = fb_.next(), mx = mix_.next(), w = width_.next(), duck = duck_.next();
        const float wa = wowAmt_.next(), fa = flutAmt_.next(), drv = drive_.next();
        const float mod0 = wa * wowSamples * wow_.valueAt(dsp::Lfo::Shape::Sine, 0.0f) +
                           fa * flutSamples * flutter_.valueAt(dsp::Lfo::Shape::Sine, 0.0f);
        const float mod1 = wa * wowSamples * wow_.valueAt(dsp::Lfo::Shape::Sine, 0.13f) +
                           fa * flutSamples * flutter_.valueAt(dsp::Lfo::Shape::Sine, 0.21f);
        wow_.advance();
        flutter_.advance();

        // Read (before writing this sample): delay T samples == read(T - 1).
        float y[2];
        y[0] = ch_[0].line.readHermite(std::clamp(ch_[0].delay.next() + mod0 - 1.0f, 1.0f, maxD));
        y[1] = ch_[1].line.readHermite(std::clamp(ch_[1].delay.next() + mod1 - 1.0f, 1.0f, maxD));

        float in[2];
        if (mode == PingPong) {
            in[0] = 0.5f * (xl + xr) + fb * y[1];
            in[1] = fb * y[0];
        } else {
            in[0] = xl + fb * y[0];
            in[1] = xr + fb * y[1];
        }
        // Tape saturation: unity small-signal gain, onset at ~1/k (k 0.5 .. 2.5).
        const float k = 0.5f + 2.0f * drv;
        for (int c = 0; c < 2; ++c) {
            float v = ch_[c].lp.process(ch_[c].hp.hp(in[c]));
            if (tape) v = std::tanh(v * k) / k;
            v = dsp::softClipKnee(v, 1.0f, 2.0f); // keeps 100 % feedback bounded
            ch_[c].line.push(v);
        }

        // Ducking: wet gain follows the input envelope.
        const float a = std::fmax(std::fabs(xl), std::fabs(xr));
        env_ += (a - env_) * (a > env_ ? envAtt_ : envRel_);
        const float dg = 1.0f / (1.0f + duck * 12.0f * env_);
        float wl = y[0] * dg, wr = y[1] * dg;
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * w;
        wl = mid + side;
        wr = mid - side;
        io.left[i] = xl + (wl - xl) * mx;
        io.right[i] = xr + (wr - xr) * mx;
    }
}

} // namespace ks
