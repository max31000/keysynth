#include "effects/drive/DriveFx.h"

#include "dsp/Math.h"
#include "dsp/SoftClip.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& DriveFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "drive";
        i.displayName = "Drive";
        i.kind = ModuleKind::Effect;
        i.category = "Distortion";
        i.params = {
            enumParam("mode", "Mode", {"Tube", "Fuzz", "Tape"}, 0),
            linearParam("drive_db", "Drive", 0.0f, 40.0f, 12.0f, "dB"),
            linearParam("asymmetry", "Asymmetry", 0.0f, 1.0f, 0.2f),
            linearParam("tone", "Tone", 0.0f, 1.0f, 0.5f),
            linearParam("level_db", "Level", -24.0f, 12.0f, 0.0f, "dB"),
            linearParam("mix", "Mix", 0.0f, 1.0f, 1.0f),
            enumParam("oversample", "Oversampling", {"Off", "2x", "4x"}, 2),
        };
        i.params.back().flags = ParamFlags::NonAutomatable;
        i.uiHints = {{"front", {"mode", "drive_db", "tone", "level_db"}}};
        return i;
    }();
    return info;
}

DriveFx::DriveFx() : Module(moduleInfo()) {}

float DriveFx::shape(int mode, float u, float bias) noexcept {
    switch (mode) {
    case Fuzz: {
        // High gain, hard knee, asymmetric with bias.
        const float v = 2.0f * u + 0.6f * bias;
        return 1.5f * dsp::softClipCubic(v);
    }
    case Tape: return std::tanh(u + 0.3f * bias);
    default: {
        // Triode-ish: the negative half clips earlier (even harmonics even without bias).
        const float v = u + 0.5f * bias;
        return v >= 0.0f ? std::tanh(v) : std::tanh(1.4f * v) * (1.0f / 1.4f);
    }
    }
}

void DriveFx::prepare(double sampleRate, int maxBlock) {
    sr_ = sampleRate;
    maxBlock_ = maxBlock;
    for (auto& c : ch_) {
        c.os.prepare(maxBlock);
        c.osDry.prepare(maxBlock);
        c.dc.prepare(sampleRate, 8.0f);
        c.tilt.setCutoff(800.0f, static_cast<float>(sampleRate));
    }
    dry_.assign(static_cast<size_t>(maxBlock), 0.0f);
    tmp_.assign(static_cast<size_t>(maxBlock) * 4, 0.0f);
    drive_.prepare(sampleRate, 0.02f);
    bias_.prepare(sampleRate, 0.02f);
    level_.prepare(sampleRate, 0.02f);
    mix_.prepare(sampleRate, 0.02f);
    tone_.prepare(sampleRate, 0.02f);
    reset();
}

void DriveFx::reset() {
    for (auto& c : ch_) {
        c.os.reset();
        c.osDry.reset();
        c.preEmph.reset();
        c.deEmph.reset();
        c.tilt.reset();
        c.dc.reset();
    }
    drive_.snap(dsp::dbToGain(params().get(DriveDb)));
    bias_.snap(params().get(Asymmetry));
    level_.snap(dsp::dbToGain(params().get(LevelDb)));
    mix_.snap(params().get(Mix));
    tone_.snap(params().get(Tone));
    lastFactor_ = 0;
}

int DriveFx::tailSamples() const { return static_cast<int>(0.2 * sr_); }

void DriveFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    const int n = io.numSamples;
    const int mode = static_cast<int>(params().get(Mode));
    const int os = static_cast<int>(params().get(Oversample));
    const int factor = os >= 2 ? 4 : (os == 1 ? 2 : 1);
    if (factor != lastFactor_) {
        const float osr = static_cast<float>(sr_) * static_cast<float>(factor);
        for (auto& c : ch_) {
            c.os.setFactor(factor);
            c.osDry.setFactor(factor);
            c.os.reset();
            c.osDry.reset();
            c.preEmph.setCutoff(3000.0f, osr);
            c.deEmph.setCutoff(1500.0f, osr);
        }
        lastFactor_ = factor;
    }
    drive_.setTarget(dsp::dbToGain(params().get(DriveDb)));
    bias_.setTarget(params().get(Asymmetry));
    level_.setTarget(dsp::dbToGain(params().get(LevelDb)));
    mix_.setTarget(params().get(Mix));
    tone_.setTarget(params().get(Tone));

    // Per-sample control values (base rate), shared by both channels.
    float* gain = tmp_.data();
    float* lvl = gain + maxBlock_;
    float* mixv = lvl + maxBlock_;
    float* hi = mixv + maxBlock_;
    for (int i = 0; i < n; ++i) {
        gain[i] = drive_.next();
        lvl[i] = level_.next() * (0.35f + 0.65f / gain[i]); // rough loudness compensation for the drive gain
        mixv[i] = mix_.next();
        hi[i] = std::exp2((tone_.next() - 0.5f) * 2.0f); // +-6 dB tilt around 800 Hz
    }
    float biasVal = 0.0f;
    for (int i = 0; i < n; ++i) biasVal = bias_.next();
    const float offset = shape(mode, 0.0f, biasVal); // static offset of the bias, removed before the DC blocker

    for (int c = 0; c < 2; ++c) {
        Channel& ch = ch_[c];
        float* x = io.channel(c);
        float* dry = dry_.data();
        // Phase-matched dry: same up/down filters, no shaping.
        ch.osDry.up(x, n);
        ch.osDry.down(dry, n);
        // Shaped path.
        ch.os.up(x, n);
        float* b = ch.os.buffer();
        const int total = n * factor;
        for (int j = 0; j < total; ++j) {
            float u = b[j] * gain[j / factor];
            if (mode == Tape) u += ch.preEmph.hp(u); // HF pre-emphasis: treble saturates first
            float y = shape(mode, u, biasVal) - offset;
            if (mode == Tape) y = 0.5f * y + 0.5f * ch.deEmph.lp(y); // matching de-emphasis
            b[j] = y;
        }
        ch.os.down(x, n);
        for (int i = 0; i < n; ++i) {
            float y = ch.dc.process(x[i]);
            const float lp = ch.tilt.lp(y);
            y = (lp / hi[i] + (y - lp) * hi[i]) * lvl[i];
            x[i] = dry[i] + (y - dry[i]) * mixv[i];
        }
    }
}

} // namespace ks
