#include "instruments/drums/DrumVoice.h"

#include <algorithm>

namespace ks::drums {

namespace {

constexpr std::array<float, 6> k808Metal = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
constexpr std::array<float, 6> k909Metal = {263.0f, 400.0f, 421.0f, 474.0f, 587.0f, 845.0f};

void setMetal(Recipe& r, const std::array<float, 6>& hz, float mul, float tr, int count = 6) {
    r.metalCount = count;
    for (int i = 0; i < count; ++i) r.metalHz[static_cast<size_t>(i)] = hz[static_cast<size_t>(i)] * mul * tr;
}

} // namespace

Recipe makeRecipe(Instr instr, Model model, float tuneSt, float ds, float tone, float vel, bool rideBell) {
    Recipe r;
    const float tr = std::exp2(tuneSt / 12.0f);
    const float tm = std::exp2((tone - 0.5f) * 2.0f); // 0.5 .. 2
    const float tl = 2.0f * tone;                     // 0 .. 2
    const float vb = 0.75f + 0.25f * vel;             // brightness with velocity
    using M = dsp::Svf::Mode;
    switch (instr) {
    case Instr::Kick:
        switch (model) {
        case Model::Tr808: // bridged-T: long boom, small pitch blip, soft click
            r.bodyHz = 49.0f * tr; r.bodyLevel = 1.0f; r.sweep = 1.0f; r.sweepTau = 0.02f;
            r.bodyTau = 0.5f * ds;
            r.clickLevel = 0.25f * tl * vel; r.clickTau = 0.0015f; r.clickCut = 3000.0f;
            r.gain = 0.9f;
            break;
        case Model::Tr909: // strong sweep from ~240 Hz, shaped body, noise click
            r.bodyHz = 54.0f * tr; r.bodyLevel = 1.0f; r.sweep = 3.5f; r.sweepTau = 0.018f; r.bodyTri = true;
            r.bodyTau = 0.3f * ds;
            r.clickLevel = 0.6f * tl * vel; r.clickTau = 0.003f; r.clickCut = 6000.0f;
            r.gain = 0.8f;
            break;
        case Model::Linn: // punchy, short, a bit of sustained boom
            r.bodyHz = 58.0f * tr; r.bodyLevel = 1.0f; r.sweep = 2.5f; r.sweepTau = 0.01f;
            r.bodyTau = 0.16f * ds; r.bodyTau2 = 0.3f * ds; r.bodyMix2 = 0.08f;
            r.clickLevel = 0.5f * tl * vel; r.clickTau = 0.002f; r.clickCut = 5000.0f;
            r.drive = 1.5f; r.gain = 0.85f;
            break;
        default: // Industrial: distorted, tight, gated
            r.bodyHz = 50.0f * tr; r.bodyLevel = 1.0f; r.sweep = 5.0f; r.sweepTau = 0.02f;
            r.bodyTau = 0.22f * ds;
            r.clickLevel = 0.8f * vel; r.clickTau = 0.004f; r.clickCut = 8000.0f;
            r.drive = 4.0f + 6.0f * tone; r.postLp = 4500.0f * tm; r.gate = 0.3f * ds;
            r.gain = 0.7f;
            break;
        }
        break;
    case Instr::Snare:
        switch (model) {
        case Model::Tr808:
            r.bodyHz = 238.0f * tr; r.bodyLevel = 1.0f; r.body2Ratio = 2.0f; r.body2Level = 0.5f;
            r.sweep = 0.1f; r.sweepTau = 0.01f; r.bodyTau = 0.045f * ds;
            r.noiseLevel = 0.6f * tl; r.nMode = M::HighPass; r.nCut = 1800.0f * tm * vb; r.nRes = 0.1f;
            r.noiseTau = 0.11f * ds; r.gain = 0.6f;
            break;
        case Model::Tr909:
            r.bodyHz = 190.0f * tr; r.bodyLevel = 1.0f; r.body2Ratio = 1.72f; r.body2Level = 0.6f;
            r.sweep = 0.35f; r.sweepTau = 0.015f; r.bodyTau = 0.07f * ds;
            r.noiseLevel = 0.9f * tl; r.nMode = M::LowPass; r.nCut = 7000.0f * tm * vb; r.hpCut = 600.0f;
            r.noiseTau = 0.16f * ds; r.gain = 0.55f;
            break;
        case Model::Linn:
            r.bodyHz = 200.0f * tr; r.bodyLevel = 1.0f; r.body2Ratio = 1.6f; r.body2Level = 0.4f;
            r.sweep = 0.2f; r.sweepTau = 0.008f; r.bodyTau = 0.06f * ds;
            r.noiseLevel = 0.8f * tl; r.nMode = M::BandPass; r.nCut = 3500.0f * tm * vb; r.nRes = 0.1f;
            r.hpCut = 400.0f; r.noiseTau = 0.12f * ds; r.noiseTau2 = 0.25f * ds; r.noiseMix2 = 0.06f;
            r.clickLevel = 0.3f * vel; r.clickTau = 0.001f; r.clickCut = 8000.0f;
            r.gain = 0.6f;
            break;
        default: // Industrial: big, saturated, gated tail
            r.bodyHz = 175.0f * tr; r.bodyLevel = 1.0f; r.body2Ratio = 1.5f; r.body2Level = 0.6f;
            r.sweep = 0.6f; r.sweepTau = 0.02f; r.bodyTau = 0.12f * ds;
            r.noiseLevel = 1.0f * std::max(0.3f, tl); r.nMode = M::BandPass; r.nCut = 2500.0f * tm * vb;
            r.nRes = 0.05f; r.hpCut = 300.0f; r.noiseTau = 0.22f * ds; r.noiseTau2 = 0.5f * ds; r.noiseMix2 = 0.25f;
            r.clickLevel = 0.4f * vel; r.clickTau = 0.002f; r.clickCut = 6000.0f;
            r.drive = 3.0f + 3.0f * tone; r.gate = 0.35f * ds; r.gain = 0.5f;
            break;
        }
        break;
    case Instr::Clap:
        r.noiseLevel = 1.0f; r.nMode = M::BandPass;
        switch (model) {
        case Model::Tr808:
            r.nCut = 1000.0f * tm * tr; r.nRes = 0.35f; r.hpCut = 500.0f;
            r.bursts = 4; r.burstSpacing = 0.0095f; r.burstTau = 0.0035f; r.noiseTau = 0.12f * ds; r.gain = 0.9f;
            break;
        case Model::Tr909:
            r.nCut = 1200.0f * tm * tr; r.nRes = 0.3f; r.hpCut = 700.0f;
            r.bursts = 4; r.burstSpacing = 0.0085f; r.burstTau = 0.003f; r.noiseTau = 0.16f * ds; r.gain = 0.9f;
            break;
        case Model::Linn:
            r.nCut = 1500.0f * tm * tr; r.nRes = 0.25f; r.hpCut = 800.0f;
            r.bursts = 3; r.burstSpacing = 0.011f; r.burstTau = 0.004f; r.noiseTau = 0.14f * ds;
            r.noiseTau2 = 0.4f * ds; r.noiseMix2 = 0.1f; r.gain = 0.9f;
            break;
        default:
            r.nCut = 1100.0f * tm * tr; r.nRes = 0.2f; r.hpCut = 400.0f;
            r.bursts = 5; r.burstSpacing = 0.008f; r.burstTau = 0.004f; r.noiseTau = 0.25f * ds;
            r.drive = 3.0f; r.gate = 0.3f * ds; r.gain = 0.7f;
            break;
        }
        break;
    case Instr::ClosedHat:
    case Instr::OpenHat: {
        const bool open = instr == Instr::OpenHat;
        switch (model) {
        case Model::Tr808:
            setMetal(r, k808Metal, 1.0f, tr); r.metalLevel = 1.0f / 6.0f; r.noiseLevel = 0.1f;
            r.nMode = M::BandPass; r.nCut = 10000.0f * tm * vb; r.nRes = 0.2f; r.hpCut = 7000.0f;
            r.noiseTau = (open ? 0.28f : 0.035f) * ds; r.gain = 1.1f;
            break;
        case Model::Tr909:
            setMetal(r, k909Metal, 1.0f, tr); r.metalLevel = 0.1f; r.noiseLevel = 0.5f;
            r.nMode = M::BandPass; r.nCut = 11000.0f * tm * vb; r.nRes = 0.1f; r.hpCut = 8000.0f;
            r.noiseTau = (open ? 0.35f : 0.045f) * ds; r.gain = 1.0f;
            break;
        case Model::Linn:
            setMetal(r, k808Metal, 1.3f, tr); r.metalLevel = 0.05f; r.noiseLevel = 1.0f;
            r.nMode = M::BandPass; r.nCut = 12000.0f * tm * vb; r.nRes = 0.05f; r.hpCut = 9000.0f;
            r.noiseTau = (open ? 0.25f : 0.03f) * ds; r.gain = 1.0f;
            break;
        default:
            setMetal(r, k808Metal, 1.5f, tr); r.metalLevel = 1.0f / 6.0f; r.noiseLevel = 0.4f;
            r.nMode = M::BandPass; r.nCut = 7000.0f * tm * vb; r.nRes = 0.3f; r.hpCut = 5000.0f;
            r.noiseTau = (open ? 0.3f : 0.04f) * ds; r.drive = 3.0f; r.gain = 0.8f;
            break;
        }
        break;
    }
    case Instr::Crash:
        r.nMode = M::BandPass; r.noiseTau2 = 0.05f; r.noiseMix2 = 0.45f;
        switch (model) {
        case Model::Tr808:
            setMetal(r, k808Metal, 1.0f, tr); r.metalLevel = 1.0f / 6.0f; r.noiseLevel = 0.5f;
            r.nCut = 6500.0f * tm * vb; r.nRes = 0.1f; r.hpCut = 3500.0f; r.noiseTau = 0.4f * ds; r.gain = 0.8f;
            break;
        case Model::Tr909:
            setMetal(r, k909Metal, 1.2f, tr); r.metalLevel = 0.07f; r.noiseLevel = 0.8f;
            r.nCut = 9000.0f * tm * vb; r.nRes = 0.05f; r.hpCut = 5000.0f; r.noiseTau = 0.45f * ds; r.gain = 0.8f;
            break;
        case Model::Linn:
            setMetal(r, k808Metal, 1.4f, tr); r.metalLevel = 0.05f; r.noiseLevel = 0.9f;
            r.nCut = 8000.0f * tm * vb; r.nRes = 0.05f; r.hpCut = 4500.0f; r.noiseTau = 0.42f * ds; r.gain = 0.8f;
            break;
        default:
            setMetal(r, k808Metal, 1.2f, tr); r.metalLevel = 1.0f / 6.0f; r.noiseLevel = 0.8f;
            r.nCut = 5000.0f * tm * vb; r.nRes = 0.1f; r.hpCut = 3000.0f; r.noiseTau = 0.45f * ds;
            r.drive = 2.5f; r.gain = 0.6f;
            break;
        }
        break;
    case Instr::Ride: {
        const float mul = model == Model::Tr808 ? 1.6f : model == Model::Tr909 ? 1.75f : model == Model::Linn ? 1.7f : 1.5f;
        setMetal(r, model == Model::Tr909 ? k909Metal : k808Metal, mul, tr);
        r.metalLevel = 0.8f / 6.0f; r.noiseLevel = 0.2f;
        r.nMode = M::BandPass; r.nCut = 9000.0f * tm * vb; r.nRes = 0.15f; r.hpCut = 5000.0f;
        r.noiseTau = 0.5f * ds; r.noiseTau2 = 0.03f; r.noiseMix2 = 0.3f;
        r.bodyHz = 3100.0f * tr; r.bodyLevel = rideBell ? 0.5f : 0.12f; r.body2Ratio = 1.48f; r.body2Level = 0.5f;
        r.bodyTau = (rideBell ? 0.6f : 0.3f) * ds;
        if (model == Model::Industrial) r.drive = 2.0f;
        r.gain = 0.8f;
        break;
    }
    case Instr::TomLo:
    case Instr::TomMid:
    case Instr::TomHi: {
        const int k = instr == Instr::TomLo ? 0 : instr == Instr::TomMid ? 1 : 2;
        const float tauMul = k == 0 ? 1.15f : k == 1 ? 1.0f : 0.85f;
        static constexpr float hz[4][3] = {{80, 120, 165}, {100, 145, 200}, {92, 132, 180}, {85, 125, 170}};
        r.bodyHz = hz[static_cast<int>(model)][k] * tr; r.bodyLevel = 1.0f;
        r.sweep = model == Model::Tr808 ? 0.4f : 0.6f; r.sweepTau = 0.04f;
        r.gain = 0.8f;
        switch (model) {
        case Model::Tr808: r.bodyTau = 0.3f * ds * tauMul; r.noiseLevel = 0.05f * tl; r.nMode = M::LowPass;
            r.nCut = 3000.0f; r.noiseTau = 0.02f; break;
        case Model::Tr909: r.bodyTau = 0.25f * ds * tauMul; r.noiseLevel = 0.15f * tl; r.nMode = M::LowPass;
            r.nCut = 4000.0f * tm; r.noiseTau = 0.03f; r.clickLevel = 0.3f * vel; r.clickCut = 5000.0f; break;
        case Model::Linn: r.bodyTau = 0.22f * ds * tauMul; r.noiseLevel = 0.2f * tl; r.nMode = M::BandPass;
            r.nCut = 2000.0f * tm; r.nRes = 0.1f; r.noiseTau = 0.04f; r.bodyTri = true; break;
        default: r.bodyTau = 0.25f * ds * tauMul; r.noiseLevel = 0.25f * tl; r.nMode = M::BandPass;
            r.nCut = 1800.0f * tm; r.noiseTau = 0.05f; r.drive = 3.0f; r.gain = 0.6f; break;
        }
        break;
    }
    case Instr::Rim:
        switch (model) {
        case Model::Tr808:
            r.bodyHz = 1667.0f * tr; r.bodyLevel = 0.7f; r.body2Ratio = 455.0f / 1667.0f; r.body2Level = 1.0f;
            r.bodyTau = 0.008f * ds; r.clickLevel = 0.4f; r.clickTau = 0.002f; r.clickCut = 6000.0f * tm;
            r.gain = 0.6f;
            break;
        case Model::Tr909:
            r.bodyHz = 1100.0f * tr; r.bodyLevel = 0.8f; r.body2Ratio = 0.4f; r.body2Level = 0.7f;
            r.bodyTau = 0.01f * ds; r.noiseLevel = 0.5f; r.nMode = M::BandPass; r.nCut = 4000.0f * tm;
            r.noiseTau = 0.006f * ds; r.gain = 0.6f;
            break;
        case Model::Linn: // sidestick
            r.bodyHz = 820.0f * tr; r.bodyLevel = 0.8f; r.body2Ratio = 1.9f; r.body2Level = 0.5f;
            r.bodyTau = 0.018f * ds; r.noiseLevel = 0.6f; r.nMode = M::BandPass; r.nCut = 2200.0f * tm;
            r.nRes = 0.3f; r.noiseTau = 0.02f * ds; r.gain = 0.6f;
            break;
        default: // metallic clank
            r.metalCount = 2; r.metalHz = {1250.0f * tr, 1870.0f * tr, 0, 0, 0, 0}; r.metalLevel = 0.5f;
            r.bodyHz = 900.0f * tr; r.bodyLevel = 0.6f; r.bodyTau = 0.03f * ds;
            r.noiseLevel = 0.6f; r.nMode = M::BandPass; r.nCut = 3000.0f * tm; r.noiseTau = 0.04f * ds;
            r.drive = 4.0f; r.gain = 0.5f;
            break;
        }
        break;
    case Instr::Cowbell:
        r.nMode = M::BandPass; r.noiseMix2 = 0.6f; r.noiseTau2 = 0.012f;
        switch (model) {
        case Model::Tr808: // two squares 540 / 800 Hz, band-passed, 2-stage decay
            r.metalCount = 2; r.metalHz = {540.0f * tr, 800.0f * tr, 0, 0, 0, 0}; r.metalLevel = 0.5f;
            r.nCut = 1400.0f * tm * tr; r.nRes = 0.3f; r.noiseTau = 0.12f * ds; r.gain = 0.9f;
            break;
        case Model::Tr909:
            r.metalCount = 2; r.metalHz = {587.0f * tr, 845.0f * tr, 0, 0, 0, 0}; r.metalLevel = 0.5f;
            r.nCut = 1600.0f * tm * tr; r.nRes = 0.25f; r.noiseTau = 0.1f * ds; r.noiseMix2 = 0.5f;
            r.noiseTau2 = 0.01f; r.gain = 0.9f;
            break;
        case Model::Linn:
            r.metalCount = 2; r.metalHz = {560.0f * tr, 835.0f * tr, 0, 0, 0, 0}; r.metalLevel = 0.5f;
            r.nCut = 1800.0f * tm * tr; r.nRes = 0.35f; r.noiseTau = 0.09f * ds; r.noiseTau2 = 0.008f; r.gain = 0.9f;
            break;
        default:
            r.metalCount = 4; r.metalHz = {540.0f * tr, 800.0f * tr, 1170.0f * tr, 1660.0f * tr, 0, 0};
            r.metalLevel = 0.3f; r.nCut = 1500.0f * tm * tr; r.nRes = 0.2f; r.noiseTau = 0.15f * ds;
            r.drive = 4.0f; r.gain = 0.6f;
            break;
        }
        break;
    case Instr::Tamb:
    default: {
        static constexpr std::array<float, 6> jingles = {3200.0f, 4300.0f, 5100.0f, 6400.0f, 7300.0f, 8900.0f};
        r.nMode = M::BandPass; r.nRes = 0.2f; r.hpCut = 5000.0f;
        switch (model) {
        case Model::Tr808: // maracas-like shaker
            r.noiseLevel = 1.0f; r.nMode = M::HighPass; r.nCut = 5000.0f * tm * tr; r.hpCut = 0.0f;
            r.noiseTau = 0.03f * ds; r.gain = 0.8f;
            break;
        case Model::Linn:
            setMetal(r, jingles, 1.0f, tr); r.metalLevel = 0.08f; r.noiseLevel = 0.7f;
            r.nCut = 8000.0f * tm * vb; r.bursts = 3; r.burstSpacing = 0.016f; r.burstTau = 0.01f;
            r.noiseTau = 0.12f * ds; r.gain = 0.8f;
            break;
        default:
            setMetal(r, jingles, 1.0f, tr); r.metalLevel = 0.08f; r.noiseLevel = 0.7f;
            r.nCut = 8500.0f * tm * vb; r.bursts = 2; r.burstSpacing = 0.018f; r.burstTau = 0.012f;
            r.noiseTau = 0.12f * ds; r.gain = 0.8f;
            if (model == Model::Industrial) r.drive = 2.5f;
            break;
        }
        break;
    }
    }
    return r;
}

void DrumVoice::setSampleRate(double sr) noexcept {
    sr_ = static_cast<float>(sr);
    invSr_ = 1.0f / sr_;
    for (auto& m : metal_) m.setSampleRate(sr);
    nf_.setSampleRate(sr);
    hp_.setSampleRate(sr);
    clickLp_.setSampleRate(sr);
    post_.setSampleRate(sr);
}

void DrumVoice::trigger(const Recipe& r, float amp, float gainL, float gainR, uint32_t seed) noexcept {
    r_ = r;
    amp_ = amp * r.gain;
    gl_ = gainL;
    gr_ = gainR;
    rng_ = seed ? seed : 0x9E3779B9u;
    age_ = 0;
    auto dec = [&](float tau) { return tau > 0.0f ? std::exp(-1.0f / (tau * sr_)) : 0.0f; };
    pEnv_ = 1.0f;
    pDec_ = dec(r.sweepTau);
    bEnv1_ = bEnv2_ = 1.0f;
    bDec1_ = dec(r.bodyTau);
    bDec2_ = dec(r.bodyTau2);
    nEnv1_ = nEnv2_ = 1.0f;
    nDec1_ = dec(r.noiseTau);
    nDec2_ = dec(r.noiseTau2);
    cEnv_ = 1.0f;
    cDec_ = dec(r.clickTau);
    burstLen_ = std::max<int64_t>(1, static_cast<int64_t>(r.burstSpacing * sr_));
    burstEnd_ = r.bursts > 1 ? burstLen_ * (r.bursts - 1) : 0;
    burstDec_ = dec(r.burstTau);
    burstEnv_ = 1.0f;
    gateAt_ = r.gate > 0.0f ? static_cast<int64_t>(r.gate * sr_) : -1;
    gateEnv_ = 1.0f;
    gateDec_ = dec(0.004f);
    driveNorm_ = r.drive > 0.0f ? 1.0f / std::tanh(r.drive) : 1.0f;
    phase1_ = phase2_ = 0.0;
    for (int i = 0; i < r.metalCount; ++i) {
        auto& m = metal_[static_cast<size_t>(i)];
        m.setFrequency(r.metalHz[static_cast<size_t>(i)]);
        m.resetPhase(0.13f * static_cast<float>(i));
    }
    nf_.reset();
    hp_.reset();
    clickLp_.reset();
    post_.reset();
    nf_.setCutoff(r.nCut, r.nRes);
    hp_.setCutoff(std::max(20.0f, r.hpCut), 0.0f);
    clickLp_.setCutoff(r.clickCut, 0.0f);
    post_.setCutoff(r.postLp > 0.0f ? r.postLp : 20000.0f, 0.0f);
    float longest = std::max({r.bodyLevel > 0 ? r.bodyTau : 0.0f, r.bodyMix2 > 0 ? r.bodyTau2 : 0.0f,
                              (r.noiseLevel > 0 || r.metalCount > 0) ? r.noiseTau : 0.0f,
                              (r.noiseMix2 > 0) ? r.noiseTau2 : 0.0f, r.clickLevel > 0 ? r.clickTau : 0.0f, 0.005f});
    float lenSec = 6.0f * longest + static_cast<float>(burstEnd_) * invSr_;
    if (r.gate > 0.0f) lenSec = std::min(lenSec, r.gate + 0.05f);
    length_ = std::max<int64_t>(16, static_cast<int64_t>(lenSec * sr_));
    invLength_ = 1.0f / static_cast<float>(length_);
    choke_ = 1.0f;
    chokeStep_ = 0.0f;
    active_ = true;
}

void DrumVoice::choke() noexcept {
    if (!active_) return;
    chokeStep_ = 1.0f / (0.004f * sr_);
}

void DrumVoice::render(float* left, float* right, int n) noexcept {
    if (!active_) return;
    const Recipe& r = r_;
    const bool hasBody = r.bodyLevel > 0.0f;
    const bool hasNoisePath = r.noiseLevel > 0.0f || r.metalCount > 0;
    const bool hasClick = r.clickLevel > 0.0f;
    const bool hasHp = r.hpCut > 0.0f;
    const double twoPi = 2.0 * 3.14159265358979323846;
    for (int i = 0; i < n; ++i) {
        float x = 0.0f;
        if (hasBody) {
            const float f = r.bodyHz * (1.0f + r.sweep * pEnv_);
            pEnv_ *= pDec_;
            phase1_ += static_cast<double>(f * invSr_);
            if (phase1_ >= 1.0) phase1_ -= 1.0;
            float b = static_cast<float>(std::sin(twoPi * phase1_));
            if (r.bodyTri) b = std::tanh(2.0f * b) * 1.0373f; // 1 / tanh(2)
            if (r.body2Level > 0.0f) {
                phase2_ += static_cast<double>(f * r.body2Ratio * invSr_);
                if (phase2_ >= 1.0) phase2_ -= 1.0;
                b += r.body2Level * static_cast<float>(std::sin(twoPi * phase2_));
            }
            const float env = (1.0f - r.bodyMix2) * bEnv1_ + r.bodyMix2 * bEnv2_;
            bEnv1_ *= bDec1_;
            bEnv2_ *= bDec2_;
            x += r.bodyLevel * b * env;
        }
        if (hasNoisePath) {
            float m = r.noiseLevel > 0.0f ? noise() * r.noiseLevel : 0.0f;
            for (int k = 0; k < r.metalCount; ++k) m += metal_[static_cast<size_t>(k)].next(dsp::Wave::Square) * r.metalLevel;
            float y = nf_.process(m, r.nMode);
            if (hasHp) y = hp_.process(y, dsp::Svf::Mode::HighPass);
            float env;
            if (age_ < burstEnd_) {
                if (age_ % burstLen_ == 0) burstEnv_ = 1.0f;
                env = burstEnv_;
                burstEnv_ *= burstDec_;
            } else {
                env = (1.0f - r.noiseMix2) * nEnv1_ + r.noiseMix2 * nEnv2_;
                nEnv1_ *= nDec1_;
                nEnv2_ *= nDec2_;
            }
            x += y * env;
        }
        if (hasClick) {
            x += clickLp_.process(noise(), dsp::Svf::Mode::LowPass) * cEnv_ * r.clickLevel * 2.0f;
            cEnv_ *= cDec_;
        }
        if (r.drive > 0.0f) x = std::tanh(x * r.drive) * driveNorm_;
        if (r.postLp > 0.0f) x = post_.process(x, dsp::Svf::Mode::LowPass);
        if (gateAt_ >= 0 && age_ >= gateAt_) gateEnv_ *= gateDec_;
        float w = 1.0f - static_cast<float>(age_) * invLength_;
        w *= w;
        const float s = x * amp_ * w * gateEnv_ * choke_;
        left[i] += s * gl_;
        right[i] += s * gr_;
        if (chokeStep_ > 0.0f) {
            choke_ -= chokeStep_;
            if (choke_ <= 0.0f) {
                active_ = false;
                return;
            }
        }
        if (++age_ >= length_) {
            active_ = false;
            return;
        }
    }
    // Early end once every envelope is inaudible.
    const float b = hasBody ? std::max(bEnv1_ * (1.0f - r.bodyMix2), bEnv2_ * r.bodyMix2) : 0.0f;
    const float nz = hasNoisePath && age_ >= burstEnd_ ? std::max(nEnv1_, nEnv2_ * r.noiseMix2) : (hasNoisePath ? 1.0f : 0.0f);
    const float c = hasClick ? cEnv_ : 0.0f;
    if (std::max({b, nz, c}) * gateEnv_ < 1e-5f) active_ = false;
}

} // namespace ks::drums
