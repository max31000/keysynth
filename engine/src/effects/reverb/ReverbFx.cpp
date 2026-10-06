#include "effects/reverb/ReverbFx.h"

#include "dsp/Math.h"
#include "dsp/SoftClip.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {

// Base FDN line lengths in ms (incommensurate, ~geometric spread 30..93 ms).
constexpr std::array<float, ReverbFx::kLines> kLineMs = {31.31f, 33.97f, 36.83f, 39.71f, 42.43f, 45.67f,
                                                          48.89f, 52.37f, 56.11f, 59.93f, 64.19f, 68.53f,
                                                          73.39f, 78.17f, 83.69f, 92.83f};
// Injection / output sign patterns (rows of a 16x16 Hadamard-like +-1 set, mutually ~orthogonal).
constexpr std::array<float, ReverbFx::kLines> kInL = {1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 1, 0, -1, 0, -1, 0};
constexpr std::array<float, ReverbFx::kLines> kInR = {0, 1, 0, 1, 0, -1, 0, 1, 0, -1, 0, -1, 0, 1, 0, -1};
constexpr std::array<float, ReverbFx::kLines> kOutL = {1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, -1, -1};
constexpr std::array<float, ReverbFx::kLines> kOutR = {1, 1, -1, 1, 1, -1, -1, 1, -1, 1, 1, 1, -1, -1, 1, -1};
// Early reflection taps (ms, before scaling) and diffuser lengths (ms).
constexpr std::array<float, ReverbFx::kErTaps> kErL = {7.1f, 11.3f, 16.9f, 21.7f, 28.3f, 33.9f, 41.3f, 49.1f};
constexpr std::array<float, ReverbFx::kErTaps> kErR = {8.3f, 12.7f, 15.1f, 23.9f, 26.9f, 36.1f, 43.7f, 51.9f};
constexpr std::array<float, ReverbFx::kDiffusers> kDiffL = {4.771f, 3.595f, 12.73f, 9.307f};
constexpr std::array<float, ReverbFx::kDiffusers> kDiffR = {4.533f, 3.311f, 11.97f, 9.829f};
constexpr float kMaxScale = 1.5f;     // lengthScale(mode) * (0.5 + size) <= 1.0 * 1.5
constexpr float kMaxModMs = 0.6f;
constexpr float kErMaxMs = 52.0f * kMaxScale;
constexpr float kLfCrossoverHz = 250.0f;

// In-place fast Walsh-Hadamard transform, normalized (orthogonal).
inline void fwht16(float* v) noexcept {
    for (int h = 1; h < 16; h <<= 1)
        for (int i = 0; i < 16; i += h << 1)
            for (int j = i; j < i + h; ++j) {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    for (int i = 0; i < 16; ++i) v[i] *= 0.25f;
}

} // namespace

const ModuleInfo& ReverbFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "reverb";
        i.displayName = "Reverb";
        i.kind = ModuleKind::Effect;
        i.category = "Reverb";
        i.params = {
            enumParam("mode", "Mode", {"Hall", "Plate", "Room", "Chamber", "Gated", "Shimmer"}, 0),
            linearParam("mix", "Mix", 0.0f, 1.0f, 0.25f),
            linearParam("predelay_ms", "Pre-delay", 0.0f, kMaxPredelayMs, 20.0f, "ms"),
            linearParam("size", "Size", 0.0f, 1.0f, 0.5f),
            logParam("decay", "Decay (RT60)", 0.1f, 20.0f, 2.5f, "s", {}, 2.0f),
            linearParam("damp_hf", "HF Damping", 0.0f, 1.0f, 0.4f, {}, "Tone"),
            linearParam("damp_lf", "LF Damping", 0.0f, 1.0f, 0.1f, {}, "Tone"),
            linearParam("early", "Early Reflections", 0.0f, 1.0f, 0.5f),
            linearParam("diffusion", "Diffusion", 0.0f, 1.0f, 0.7f),
            linearParam("mod", "Modulation", 0.0f, 1.0f, 0.4f),
            linearParam("width", "Width", 0.0f, 1.0f, 1.0f),
            logParam("low_cut_hz", "Low Cut", 20.0f, 1000.0f, 20.0f, "Hz", "Tone", 120.0f),
            logParam("high_cut_hz", "High Cut", 1000.0f, 20000.0f, 16000.0f, "Hz", "Tone", 6000.0f),
            linearParam("gate_threshold_db", "Gate Threshold", -60.0f, 0.0f, -30.0f, "dB", "Gate"),
            logParam("gate_hold_ms", "Gate Hold", 10.0f, 1000.0f, 250.0f, "ms", "Gate", 200.0f),
            linearParam("shimmer", "Shimmer", 0.0f, 1.0f, 0.4f, {}, "Shimmer"),
        };
        i.uiHints = {{"front", {"mode", "mix", "size", "decay", "predelay_ms"}}};
        return i;
    }();
    return info;
}

const ReverbFx::ModeShape& ReverbFx::shape(int mode) noexcept {
    //                                lenScale erScale erGain diffusion mod  crossover
    static constexpr ModeShape kHall{1.00f, 1.00f, 0.60f, 1.00f, 1.0f, 4000.0f};
    static constexpr ModeShape kPlate{0.55f, 0.30f, 0.00f, 1.25f, 0.6f, 6500.0f};
    static constexpr ModeShape kRoom{0.30f, 0.35f, 1.00f, 0.90f, 0.5f, 3000.0f};
    static constexpr ModeShape kChamber{0.55f, 0.60f, 0.80f, 1.00f, 0.6f, 3500.0f};
    static constexpr ModeShape kGated{0.45f, 0.50f, 0.80f, 1.10f, 0.3f, 5000.0f};
    static constexpr ModeShape kShimmer{1.00f, 1.00f, 0.40f, 1.00f, 1.0f, 5000.0f};
    switch (mode) {
    case Plate: return kPlate;
    case Room: return kRoom;
    case Chamber: return kChamber;
    case Gated: return kGated;
    case ShimmerMode: return kShimmer;
    default: return kHall;
    }
}

ReverbFx::ReverbFx() : Module(moduleInfo()) {}

float ReverbFx::PitchShifter::process(float x) noexcept {
    line.push(x);
    phase -= 1.0f / window;
    if (phase < 0.0f) phase += 1.0f;
    float p2 = phase + 0.5f;
    if (p2 >= 1.0f) p2 -= 1.0f;
    const float g1 = 1.0f - std::fabs(2.0f * phase - 1.0f);
    const float g2 = 1.0f - std::fabs(2.0f * p2 - 1.0f);
    return g1 * line.readLinear(1.0f + phase * window) + g2 * line.readLinear(1.0f + p2 * window);
}

void ReverbFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    const float msToS = 0.001f * static_cast<float>(sampleRate);
    for (auto& p : pre_) p.prepare(static_cast<int>((kMaxPredelayMs + kErMaxMs + 5.0f) * msToS));
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < kDiffusers; ++k) {
            auto& d = diff_[c][static_cast<size_t>(k)];
            d.length = std::max(2, static_cast<int>((c == 0 ? kDiffL : kDiffR)[static_cast<size_t>(k)] * msToS));
            d.line.prepare(d.length + 2);
        }
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[static_cast<size_t>(i)];
        l.delay.prepare(static_cast<int>((kLineMs[static_cast<size_t>(i)] * kMaxScale + kMaxModMs + 2.0f) * msToS));
        const float rate = 0.11f + 0.071f * static_cast<float>(i); // 0.11 .. 1.18 Hz
        const float w = dsp::kTwoPi * rate / static_cast<float>(sampleRate);
        l.rotC = std::cos(w);
        l.rotS = std::sin(w);
    }
    shifter_.window = std::round(0.045f * static_cast<float>(sampleRate));
    shifter_.line.prepare(static_cast<int>(shifter_.window) + 4);
    mix_.prepare(sampleRate, 0.02f);
    predelay_.prepare(sampleRate, 0.1f);
    scale_.prepare(sampleRate, 0.3f);
    early_.prepare(sampleRate, 0.03f);
    width_.prepare(sampleRate, 0.03f);
    modDepth_.prepare(sampleRate, 0.1f);
    shimmer_.prepare(sampleRate, 0.05f);
    erScale_.prepare(sampleRate, 0.2f);
    erGain_.prepare(sampleRate, 0.05f);
    diffG_.prepare(sampleRate, 0.05f);
    envAtt_ = dsp::onePoleCoef(0.0005f, sampleRate);
    envRel_ = dsp::onePoleCoef(0.03f, sampleRate);
    gateAtt_ = dsp::onePoleCoef(0.001f, sampleRate);
    gateRel_ = dsp::onePoleCoef(0.012f, sampleRate);
    reset();
}

void ReverbFx::reset() {
    for (auto& p : pre_) p.reset();
    for (auto& ch : diff_)
        for (auto& d : ch) d.line.reset();
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[static_cast<size_t>(i)];
        l.delay.reset();
        l.hf.reset();
        l.lf.reset();
        const float ph = 0.37f * static_cast<float>(i);
        l.oscC = std::cos(dsp::kTwoPi * ph);
        l.oscS = std::sin(dsp::kTwoPi * ph);
    }
    shifter_.line.reset();
    shifter_.phase = 0.0f;
    for (int c = 0; c < 2; ++c) {
        wetHp_[c].reset();
        wetLp_[c].reset();
    }
    const int mode = static_cast<int>(params().get(Mode));
    const ModeShape& s = shape(mode);
    mix_.snap(params().get(Mix));
    predelay_.snap(params().get(PredelayMs));
    scale_.snap(s.lengthScale * (0.5f + params().get(Size)));
    early_.snap(params().get(Early));
    width_.snap(params().get(Width));
    modDepth_.snap(params().get(ModDepth) * s.mod);
    shimmer_.snap(mode == ShimmerMode ? params().get(Shimmer) : 0.0f);
    erScale_.snap(s.erScale * (0.5f + params().get(Size)));
    erGain_.snap(s.erGain);
    diffG_.snap(std::min(0.78f, 0.62f * params().get(Diffusion) * s.diffusion + 0.1f));
    shimFeed_ = 0.0f;
    gateEnv_ = 0.0f;
    gateGain_ = mode == Gated ? 0.0f : 1.0f;
    gateHold_ = 0;
    lastMode_ = mode;
    lastLow_ = lastHigh_ = -1.0f;
}

int ReverbFx::tailSamples() const {
    const int mode = static_cast<int>(params().get(Mode));
    const double pre = (params().get(PredelayMs) + kErMaxMs) * 0.001;
    double t;
    if (mode == Gated) {
        t = pre + params().get(GateHoldMs) * 0.001 + 0.15;
    } else {
        // -90 dB = 1.5 x RT60. Shimmer re-injection lengthens the tail.
        double decay = 1.5 * params().get(Decay);
        if (mode == ShimmerMode) decay *= 1.0 + 2.0 * params().get(Shimmer);
        t = pre + decay + 0.1;
    }
    return static_cast<int>(std::min(t, 60.0) * sr_);
}

void ReverbFx::updateLines(float lengthScale, float decay, float dampHf, float dampLf, float crossover) noexcept {
    const float sr = static_cast<float>(sr_);
    const float tMid = std::max(decay, 0.05f);
    const float tHf = tMid * (1.0f - 0.9f * dampHf);
    const float tLf = tMid * (1.0f - 0.8f * dampLf);
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[static_cast<size_t>(i)];
        const float lenS = kLineMs[static_cast<size_t>(i)] * lengthScale * 0.001f; // seconds
        // Per-pass gain for an RT60 of t: 10^(-3 len / t).
        l.gain = std::pow(10.0f, -3.0f * lenS / tMid);
        l.kHf = std::pow(10.0f, -3.0f * lenS * (1.0f / tHf - 1.0f / tMid));
        l.kLf = std::pow(10.0f, -3.0f * lenS * (1.0f / tLf - 1.0f / tMid));
        l.hf.setCutoff(crossover, sr);
        l.lf.setCutoff(kLfCrossoverHz, sr);
        // Re-normalize the quadrature oscillator (float drift).
        const float r = 1.0f / std::sqrt(l.oscC * l.oscC + l.oscS * l.oscS);
        l.oscC *= r;
        l.oscS *= r;
    }
}

void ReverbFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    const int mode = static_cast<int>(params().get(Mode));
    const ModeShape& s = shape(mode);
    const float sr = static_cast<float>(sr_);
    if (mode != lastMode_) {
        if (mode == Gated) gateGain_ = 0.0f;
        lastMode_ = mode;
    }
    const float size = params().get(Size);
    mix_.setTarget(params().get(Mix));
    predelay_.setTarget(params().get(PredelayMs));
    scale_.setTarget(s.lengthScale * (0.5f + size));
    early_.setTarget(params().get(Early));
    width_.setTarget(params().get(Width));
    modDepth_.setTarget(params().get(ModDepth) * s.mod);
    shimmer_.setTarget(mode == ShimmerMode ? params().get(Shimmer) : 0.0f);
    erScale_.setTarget(s.erScale * (0.5f + size));
    erGain_.setTarget(s.erGain);
    diffG_.setTarget(std::min(0.78f, 0.62f * params().get(Diffusion) * s.diffusion + 0.1f));
    const float decay = params().get(Decay);
    updateLines(scale_.value(), decay, params().get(DampHf), params().get(DampLf), s.crossoverHz);
    const float low = params().get(LowCutHz), high = params().get(HighCutHz);
    if (low != lastLow_ || high != lastHigh_) {
        for (int c = 0; c < 2; ++c) {
            wetHp_[c].setCutoff(low, sr);
            wetLp_[c].setCutoff(high, sr);
        }
        lastLow_ = low;
        lastHigh_ = high;
    }
    // Late level: keeps the steady-state loudness from growing without bound with RT60.
    const float lateGain = 0.55f / std::sqrt(0.5f + 0.35f * decay);
    const bool gated = mode == Gated;
    const float gateThr = dsp::dbToGain(params().get(GateThresholdDb));
    const int holdSamples = static_cast<int>(params().get(GateHoldMs) * 0.001f * sr);
    const float msToS = 0.001f * sr;
    constexpr float kErNorm = 0.42f; // ~1/sqrt(sum g^2) for 0.85^k taps

    for (int n = 0; n < io.numSamples; ++n) {
        const float xl = io.left[n], xr = io.right[n];
        pre_[0].push(xl);
        pre_[1].push(xr);
        const float pd = predelay_.next() * msToS;
        const float pl = pre_[0].readLinear(pd), pr = pre_[1].readLinear(pd);

        // Early reflections.
        const float ers = erScale_.next() * msToS, erg = erGain_.next() * early_.next() * kErNorm;
        float el = 0.0f, er = 0.0f, g = 1.0f;
        for (int k = 0; k < kErTaps; ++k) {
            const float sign = (k & 1) ? -1.0f : 1.0f;
            el += sign * g * pre_[0].readLinear(pd + kErL[static_cast<size_t>(k)] * ers);
            er += sign * g * pre_[1].readLinear(pd + kErR[static_cast<size_t>(k)] * ers);
            g *= 0.85f;
        }
        el *= erg;
        er *= erg;

        // Input diffusion (+ shimmer re-injection).
        const float dg = diffG_.next();
        float dl = pl + shimFeed_, dr = pr + shimFeed_;
        for (int k = 0; k < kDiffusers; ++k) {
            dl = diff_[0][static_cast<size_t>(k)].process(dl, dg);
            dr = diff_[1][static_cast<size_t>(k)].process(dr, dg);
        }

        // FDN.
        const float sc = scale_.next() * msToS;
        const float md = modDepth_.next() * kMaxModMs * msToS;
        float y[kLines], z[kLines];
        float ol = 0.0f, orr = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines_[static_cast<size_t>(i)];
            const float c = l.oscC * l.rotC - l.oscS * l.rotS;
            l.oscS = l.oscS * l.rotC + l.oscC * l.rotS;
            l.oscC = c;
            const float d = kLineMs[static_cast<size_t>(i)] * sc + md * (1.0f + l.oscS) - 1.0f;
            y[i] = l.delay.readHermite(d);
            ol += kOutL[static_cast<size_t>(i)] * y[i];
            orr += kOutR[static_cast<size_t>(i)] * y[i];
            // Absorption: HF shelf above the crossover, LF shelf below 250 Hz, then the mid gain.
            const float lpH = l.hf.lp(y[i]);
            float v = lpH + l.kHf * (y[i] - lpH);
            const float lpL = l.lf.lp(v);
            v = (v - lpL) + l.kLf * lpL;
            z[i] = l.gain * v;
        }
        fwht16(z);
        for (int i = 0; i < kLines; ++i)
            lines_[static_cast<size_t>(i)].delay.push(
                z[i] + 0.5f * (kInL[static_cast<size_t>(i)] * dl + kInR[static_cast<size_t>(i)] * dr));

        float wl = ol * 0.25f * lateGain, wr = orr * 0.25f * lateGain;

        // Shimmer: octave-up of the late sound back into the tank (soft-limited => bounded).
        const float sh = shimmer_.next();
        if (sh > 0.0f) {
            shimFeed_ = dsp::softClipKnee(0.6f * sh * shifter_.process(0.5f * (wl + wr)), 0.5f, 1.0f);
        } else {
            shimFeed_ = 0.0f;
        }

        wl += el;
        wr += er;

        // Gate (Gated mode): keyed by the pre-delayed input.
        if (gated) {
            const float a = std::fmax(std::fabs(pl), std::fabs(pr));
            gateEnv_ += (a - gateEnv_) * (a > gateEnv_ ? envAtt_ : envRel_);
            float target = 0.0f;
            if (gateEnv_ > gateThr) {
                gateHold_ = holdSamples;
                target = 1.0f;
            } else if (gateHold_ > 0) {
                --gateHold_;
                target = 1.0f;
            }
            gateGain_ += (target - gateGain_) * (target > gateGain_ ? gateAtt_ : gateRel_);
            wl *= gateGain_;
            wr *= gateGain_;
        }

        wl = wetLp_[0].lp(wetHp_[0].hp(wl));
        wr = wetLp_[1].lp(wetHp_[1].hp(wr));
        const float w = width_.next();
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * w;
        wl = mid + side;
        wr = mid - side;
        const float mx = mix_.next();
        io.left[n] = xl + (wl - xl) * mx;
        io.right[n] = xr + (wr - xr) * mx;
    }
}

} // namespace ks
