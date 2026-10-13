#include "effects/vocoder/VocoderFx.h"

#include "dsp/Math.h"
#include "dsp/SoftClip.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
constexpr int kBandCounts[4] = {8, 16, 20, 32};
constexpr float kBandGain = 20.0f; // x sqrt(bands) / 4: wet ~ carrier level for a modulator around -16 dBFS

// Internal "Vowels" modulator: formants (Hz) of a, e, i, o, u (adult male, Peterson & Barney-like); index 5 = "s".
constexpr float kVowelF[5][3] = {
    {730.0f, 1090.0f, 2440.0f}, {530.0f, 1840.0f, 2480.0f}, {270.0f, 2290.0f, 3010.0f},
    {570.0f, 840.0f, 2410.0f},  {300.0f, 870.0f, 2240.0f},
};
constexpr double kSyllable = 0.42; // s
constexpr int kSyllables = 6;      // a e i o u s
} // namespace

const ModuleInfo& VocoderFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "vocoder";
        i.displayName = "Vocoder";
        i.kind = ModuleKind::Effect;
        i.category = "Filter";
        i.params = {
            enumParam("bands", "Bands", {"8", "16", "20", "32"}, 1),
            enumParam("modulator", "Modulator", {"Input", "Vowels (test)", "Noise"}, ModInput, "Modulator"),
            enumParam("input_channel", "Input Channel", {"Left", "Right", "Mono (L+R)"}, InMono, "Modulator"),
            linearParam("mod_gain_db", "Mod Gain", -12.0f, 40.0f, 0.0f, "dB", "Modulator"),
            linearParam("gate_db", "Gate", -96.0f, -20.0f, -70.0f, "dB", "Modulator"),
            logParam("attack_ms", "Attack", 0.5f, 100.0f, 4.0f, "ms", "Envelope", 5.0f),
            logParam("release_ms", "Release", 5.0f, 1000.0f, 50.0f, "ms", "Envelope", 60.0f),
            linearParam("formant", "Formant Shift", -12.0f, 12.0f, 0.0f, "st"),
            linearParam("sibilance", "Sibilance", 0.0f, 1.0f, 0.4f),
            linearParam("carrier_mix", "Carrier", 0.0f, 1.0f, 0.0f),
            linearParam("level_db", "Level", -24.0f, 12.0f, 0.0f, "dB"),
            linearParam("mix", "Mix", 0.0f, 1.0f, 1.0f),
        };
        i.uiHints = {{"groupOrder", {"", "Modulator", "Envelope"}},
                     {"front", {"bands", "modulator", "mod_gain_db", "formant", "sibilance", "mix"}}};
        return i;
    }();
    return info;
}

VocoderFx::VocoderFx() : Module(moduleInfo()) {}

int VocoderFx::bandCount(int bandsIndex) noexcept { return kBandCounts[std::clamp(bandsIndex, 0, 3)]; }

float VocoderFx::bandHz(int b) const noexcept {
    return b >= 0 && b < nBands_ ? centreHz_[static_cast<size_t>(b)] : 0.0f;
}

void VocoderFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    const float sr = static_cast<float>(sampleRate);
    modHf_.setSampleRate(sampleRate);
    modHf_.setGK(dsp::tptG(5000.0f, sr), 1.4f);
    for (auto& n : noiseHp_) {
        n.setSampleRate(sampleRate);
        n.setGK(dsp::tptG(3500.0f, sr), 1.2f);
    }
    hfEnv_.setTimes(0.002f, 0.04f, sampleRate);
    gateEnv_.setTimes(0.001f, 0.08f, sampleRate);
    carEnv_.setTimes(0.002f, 0.08f, sampleRate);
    gateCoef_ = dsp::onePoleCoef(0.005f, sampleRate);
    for (auto& f : formant_) f.setSampleRate(sampleRate);
    glottalLp_.setCutoff(2500.0f, sr);
    sHp_.setCutoff(4500.0f, sr);
    f1_.prepare(sampleRate, 0.03f);
    f2_.prepare(sampleRate, 0.03f);
    f3_.prepare(sampleRate, 0.03f);
    formantSm_.prepare(sampleRate, 0.03f);
    modGain_.prepare(sampleRate, 0.02f);
    sib_.prepare(sampleRate, 0.02f);
    carMix_.prepare(sampleRate, 0.02f);
    level_.prepare(sampleRate, 0.02f);
    mix_.prepare(sampleRate, 0.02f);
    reset();
}

void VocoderFx::configureBands(int n, float formantSemis) noexcept {
    const float sr = static_cast<float>(sr_);
    const float r = std::pow(kHighHz / kLowHz, 1.0f / static_cast<float>(n - 1));
    // Q of one 2nd-order section whose -3 dB width equals the band spacing; two cascaded sections narrow the
    // width by sqrt(sqrt(2) - 1) = 0.64, so each stage gets 0.64 x that Q.
    const float q = 0.64f * std::sqrt(r) / (r - 1.0f);
    const float k = 1.0f / q;
    const float shift = std::exp2(formantSemis / 12.0f);
    for (int b = 0; b < n; ++b) {
        const float f = kLowHz * std::pow(r, static_cast<float>(b));
        centreHz_[static_cast<size_t>(b)] = f;
        mod_[static_cast<size_t>(b)].set(dsp::tptG(f, sr), k);
        const float fc = std::fmin(f * shift, 0.45f * sr);
        for (auto& ch : car_) ch[static_cast<size_t>(b)].set(dsp::tptG(fc, sr), k);
    }
    if (n != nBands_) {
        for (int b = 0; b < kMaxBands; ++b) {
            mod_[static_cast<size_t>(b)].reset();
            for (auto& ch : car_) ch[static_cast<size_t>(b)].reset();
            env_[static_cast<size_t>(b)].reset();
        }
    }
    nBands_ = n;
    configuredFormant_ = formantSemis;
    bandGain_ = kBandGain * std::sqrt(static_cast<float>(n)) / 4.0f;
}

void VocoderFx::retuneCarrier(float formantSemis) noexcept {
    const float sr = static_cast<float>(sr_);
    const float shift = std::exp2(formantSemis / 12.0f);
    for (int b = 0; b < nBands_; ++b) {
        const size_t bi = static_cast<size_t>(b);
        const float fc = std::fmin(centreHz_[bi] * shift, 0.45f * sr);
        const float g = dsp::tptG(fc, sr);
        for (auto& ch : car_) ch[bi].set(g, ch[bi].k);
    }
    configuredFormant_ = formantSemis;
}

void VocoderFx::reset() {
    const float formant = std::clamp(params().get(Formant), -12.0f, 12.0f);
    nBands_ = 0;
    configureBands(bandCount(static_cast<int>(params().get(Bands))), formant);
    for (auto& m : mod_) m.reset();
    for (auto& ch : car_)
        for (auto& c : ch) c.reset();
    for (auto& e : env_) e.reset();
    modHf_.reset();
    for (auto& n : noiseHp_) n.reset();
    hfEnv_.reset();
    gateEnv_.reset();
    carEnv_.reset();
    gate_ = 0.0f;
    vPhase_ = 0.0;
    vTime_ = 0.0;
    for (auto& f : formant_) f.reset();
    glottalLp_.reset();
    sHp_.reset();
    f1_.snap(kVowelF[0][0]);
    f2_.snap(kVowelF[0][1]);
    f3_.snap(kVowelF[0][2]);
    formantSm_.snap(formant);
    modGain_.snap(dsp::dbToGain(params().get(ModGainDb)));
    sib_.snap(params().get(Sibilance));
    carMix_.snap(params().get(CarrierMix));
    level_.snap(dsp::dbToGain(params().get(LevelDb)));
    mix_.snap(params().get(Mix));
    lastAttack_ = lastRelease_ = -1.0f;
}

int VocoderFx::tailSamples() const {
    // Carrier band-pass ring of the lowest (narrowest relative to its period) band, shifted down an octave at most:
    // 2 stages of tau = Q / (pi f) to -90 dB. Envelopes do not add a tail (they only scale the carrier).
    return static_cast<int>(0.6 * sr_);
}

float VocoderFx::vowelSample() noexcept {
    const float sr = static_cast<float>(sr_);
    const double dt = 1.0 / sr_;
    vTime_ += dt;
    const int syl = static_cast<int>(std::floor(vTime_ / kSyllable)) % kSyllables;
    const double t = std::fmod(vTime_, kSyllable);
    // syllable envelope: 30 ms attack, release over the last 90 ms (gap between syllables)
    float env = static_cast<float>(std::min(t / 0.03, 1.0) * std::clamp((kSyllable - 0.02 - t) / 0.07, 0.0, 1.0));
    if (syl == 5) { // "s": high-passed noise
        const float n = modNoise_.next();
        return 0.25f * env * sHp_.hp(n);
    }
    f1_.setTarget(kVowelF[syl][0]);
    f2_.setTarget(kVowelF[syl][1]);
    f3_.setTarget(kVowelF[syl][2]);
    // glottal source: 110 Hz saw with a little vibrato, softened
    const double f0 = 110.0 * (1.0 + 0.008 * std::sin(2.0 * 3.141592653589793 * 5.2 * vTime_));
    vPhase_ += f0 * dt;
    vPhase_ -= std::floor(vPhase_);
    const float src = glottalLp_.lp(static_cast<float>(2.0 * vPhase_ - 1.0));
    const float fs[3] = {f1_.next(), f2_.next(), f3_.next()};
    constexpr float gains[3] = {1.0f, 0.55f, 0.3f};
    constexpr float k = 1.0f / 9.0f;
    float y = 0.0f;
    for (int i = 0; i < 3; ++i) {
        formant_[i].setGK(dsp::tptG(fs[i], sr), k);
        y += gains[i] * k * formant_[i].tick(src).bp;
    }
    return 2.0f * env * y; // ~ -16 dBFS RMS on the vowels
}

void VocoderFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext& ctx) {
    const ParamSet& p = params();
    const int n = bandCount(static_cast<int>(p.get(Bands)));
    formantSm_.setTarget(std::clamp(p.get(Formant), -12.0f, 12.0f));
    if (n != nBands_) configureBands(n, formantSm_.value());
    if (const float a = p.get(AttackMs), r = p.get(ReleaseMs); a != lastAttack_ || r != lastRelease_) {
        for (auto& e : env_) e.setTimes(a * 0.001f, r * 0.001f, sr_);
        lastAttack_ = a;
        lastRelease_ = r;
    }
    modGain_.setTarget(dsp::dbToGain(p.get(ModGainDb)));
    sib_.setTarget(p.get(Sibilance));
    carMix_.setTarget(p.get(CarrierMix));
    level_.setTarget(dsp::dbToGain(p.get(LevelDb)));
    mix_.setTarget(p.get(Mix));
    const float gateThr = dsp::dbToGain(p.get(GateDb));

    int src = std::clamp(static_cast<int>(p.get(Modulator)), 0, 2);
    const ExternalInput& in = ctx.input;
    if (src == ModInput && !in.present()) src = ModVowels; // no input channel open: audible fallback
    const int inCh = std::clamp(static_cast<int>(p.get(InputChannel)), 0, 2);

    // Formant shift glides: re-tune the carrier bank in 32-sample steps while the smoother moves.
    constexpr int kSub = 32;
    for (int i0 = 0; i0 < io.numSamples; i0 += kSub) {
        const int i1 = std::min(io.numSamples, i0 + kSub);
        float fsh = formantSm_.value();
        for (int j = i0; j < i1; ++j) fsh = formantSm_.next();
        if (std::fabs(fsh - configuredFormant_) > 0.005f) retuneCarrier(fsh);

        for (int i = i0; i < i1; ++i) {
            const float xl = io.left[i], xr = io.right[i];
            float m;
            if (src == ModInput) m = inCh == InLeft ? in.left[i] : (inCh == InRight ? in.right[i] : 0.5f * (in.left[i] + in.right[i]));
            else if (src == ModVowels) m = vowelSample();
            else m = 0.3f * modNoise_.next();
            m *= modGain_.next();
            if (!(std::fabs(m) < 16.0f)) m = 0.0f; // a bad driver sample (NaN/Inf/garbage) must not poison the bank

            const float ge = gateEnv_.process(m);
            gate_ += ((ge > gateThr ? 1.0f : 0.0f) - gate_) * gateCoef_;

            float wl = 0.0f, wr = 0.0f;
            for (int b = 0; b < nBands_; ++b) {
                const size_t bi = static_cast<size_t>(b);
                // band VCA: envelopes saturate softly (an overdriven modulator cannot blow up the output)
                const float e = dsp::softClipKnee(env_[bi].process(mod_[bi].process(m)), 0.3f, 0.6f) * gate_;
                wl += car_[0][bi].process(xl) * e;
                wr += car_[1][bi].process(xr) * e;
            }
            wl *= bandGain_;
            wr *= bandGain_;

            // Sibilance: consonant energy above ~5 kHz -> noise + a little of the modulator's own top band,
            // only while the carrier sounds.
            const float hf = modHf_.tick(m).hp;
            const float he = hfEnv_.process(hf) * gate_;
            // carrier present: 0 below ~ -52 dBFS (so the noise ends with the synth), 1 above ~ -18 dBFS
            const float cg = std::clamp(carEnv_.process(0.5f * (xl + xr)) * 8.0f - 0.02f, 0.0f, 1.0f);
            const float sb = sib_.next() * cg;
            const float hfPass = 0.5f * hf * gate_;
            wl += sb * (6.0f * he * noiseHp_[0].tick(noise_.next()).hp + hfPass);
            wr += sb * (6.0f * he * noiseHp_[1].tick(noise_.next()).hp + hfPass);

            const float cm = carMix_.next(), lv = level_.next(), mx = mix_.next();
            // output stage: soft ceiling at +12 dBFS (extreme gains stay bounded, normal levels untouched)
            wl = dsp::softClipKnee((wl + cm * xl) * lv, 2.0f, 4.0f);
            wr = dsp::softClipKnee((wr + cm * xr) * lv, 2.0f, 4.0f);
            io.left[i] = xl + (wl - xl) * mx;
            io.right[i] = xr + (wr - xr) * mx;
        }
    }
}

} // namespace ks
