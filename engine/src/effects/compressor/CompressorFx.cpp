#include "effects/compressor/CompressorFx.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& CompressorFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "compressor";
        i.displayName = "Compressor";
        i.kind = ModuleKind::Effect;
        i.category = "Dynamics";
        auto gr = linearParam("gr_db", "Gain Reduction", 0.0f, 60.0f, 0.0f, "dB");
        gr.flags = ParamFlags::ReadOnly;
        i.params = {
            enumParam("mode", "Mode", {"VCA", "Opto"}, 0),
            linearParam("threshold_db", "Threshold", -60.0f, 0.0f, -18.0f, "dB"),
            logParam("ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", {}, 4.0f),
            logParam("attack_ms", "Attack", 0.1f, 100.0f, 10.0f, "ms", {}, 10.0f),
            logParam("release_ms", "Release", 10.0f, 2000.0f, 150.0f, "ms", {}, 150.0f),
            linearParam("knee_db", "Knee", 0.0f, 24.0f, 6.0f, "dB"),
            linearParam("makeup_db", "Makeup", 0.0f, 24.0f, 0.0f, "dB"),
            linearParam("mix", "Mix", 0.0f, 1.0f, 1.0f),
            logParam("sc_hpf_hz", "Sidechain HPF", 20.0f, 500.0f, 20.0f, "Hz", "Sidechain", 100.0f),
            gr,
        };
        i.uiHints = {{"front", {"threshold_db", "ratio", "attack_ms", "release_ms", "makeup_db", "gr_db"}}};
        return i;
    }();
    return info;
}

CompressorFx::CompressorFx() : Module(moduleInfo()) {}

float CompressorFx::staticGainDb(float x, float t, float r, float w) noexcept {
    const float over = x - t;
    const float slope = 1.0f / std::max(r, 1.0f) - 1.0f; // <= 0
    if (w > 0.0f && 2.0f * std::fabs(over) <= w) {
        const float a = over + 0.5f * w;
        return slope * a * a / (2.0f * w);
    }
    return over > 0.0f ? slope * over : 0.0f;
}

void CompressorFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    makeup_.prepare(sampleRate, 0.02f);
    mix_.prepare(sampleRate, 0.02f);
    reset();
}

void CompressorFx::reset() {
    for (auto& h : scHp_) h.reset();
    gsDb_ = fastDb_ = slowDb_ = 0.0f;
    makeup_.snap(dsp::dbToGain(params().get(MakeupDb)));
    mix_.snap(params().get(Mix));
    lastHpf_ = -1.0f;
    params().setRaw(GainReductionDb, 0.0f);
}

void CompressorFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    const int mode = static_cast<int>(params().get(Mode));
    const float thr = params().get(ThresholdDb);
    const float ratio = params().get(Ratio);
    const float knee = mode == Opto ? std::max(params().get(KneeDb), 6.0f) : params().get(KneeDb);
    const float hpf = params().get(ScHpfHz);
    if (hpf != lastHpf_) {
        for (auto& h : scHp_) h.setCutoff(hpf, static_cast<float>(sr_));
        lastHpf_ = hpf;
    }
    const bool useHpf = hpf > 21.0f;
    makeup_.setTarget(dsp::dbToGain(params().get(MakeupDb)));
    mix_.setTarget(params().get(Mix));

    const float relMs = params().get(ReleaseMs);
    const float aAtt = std::exp(-1.0f / (std::max(params().get(AttackMs), 0.05f) * 0.001f * static_cast<float>(sr_)));
    const float aRel = std::exp(-1.0f / (std::max(relMs, 1.0f) * 0.001f * static_cast<float>(sr_)));
    // Opto: attack 10 ms; fast release 60 ms; slow stage charges over ~1 s and releases over 0.5..5 s.
    const auto coef = [this](float sec) { return std::exp(-1.0f / (sec * static_cast<float>(sr_))); };
    const float oAtt = coef(0.010f), oFastRel = coef(0.060f), oSlowAtt = coef(1.0f);
    const float oSlowRel = coef(std::clamp(relMs * 0.01f, 0.5f, 5.0f)); // release_ms 150 -> 1.5 s

    float maxGr = 0.0f;
    for (int i = 0; i < io.numSamples; ++i) {
        const float l = io.left[i], r = io.right[i];
        const float dl = useHpf ? scHp_[0].hp(l) : l;
        const float dr = useHpf ? scHp_[1].hp(r) : r;
        const float level = std::max(std::fabs(dl), std::fabs(dr));
        const float levelDb = level > 1e-6f ? 20.0f * std::log10(level) : -120.0f;
        const float gc = staticGainDb(levelDb, thr, ratio, knee);
        float g;
        if (mode == Opto) {
            fastDb_ = gc < fastDb_ ? oAtt * fastDb_ + (1.0f - oAtt) * gc : oFastRel * fastDb_ + (1.0f - oFastRel) * gc;
            slowDb_ = gc < slowDb_ ? oSlowAtt * slowDb_ + (1.0f - oSlowAtt) * gc
                                   : oSlowRel * slowDb_ + (1.0f - oSlowRel) * gc;
            // Short peaks get mostly the fast stage; sustained compression charges the slow stage, which then
            // holds the gain down after the fast part has recovered (LA-2A release). Steady state == gc.
            g = 0.65f * fastDb_ + 0.35f * slowDb_;
        } else {
            gsDb_ = gc < gsDb_ ? aAtt * gsDb_ + (1.0f - aAtt) * gc : aRel * gsDb_ + (1.0f - aRel) * gc;
            g = gsDb_;
        }
        maxGr = std::max(maxGr, -g);
        const float gain = std::pow(10.0f, g * 0.05f) * makeup_.next();
        const float mx = mix_.next();
        io.left[i] = l + (l * gain - l) * mx;
        io.right[i] = r + (r * gain - r) * mx;
    }
    params().setRaw(GainReductionDb, maxGr);
}

} // namespace ks
