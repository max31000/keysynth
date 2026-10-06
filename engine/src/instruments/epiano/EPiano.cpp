#include "instruments/epiano/EPiano.h"

#include "dsp/BeamModes.h"
#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
constexpr int kChunk = 32;                 // samples between voice control updates (damper, bend)
constexpr float kLn1000 = 6.907755279f;    // 60 dB in nepers
constexpr int kThumpMode = 6;              // mechanical mode (bus B, not through the pickup, not bent)
constexpr float kSilence = 1e-11f;         // -110 dB output power: voice is done
} // namespace

// Per-model constants. Times are T60 (s) at C4 unless noted; register scaling is applied in setupNote().
struct EPiano::ModelDef {
    bool electrostatic;       // Wurlitzer reed pickup instead of the magnetic tine pickup
    int keyLo, keyHi;         // the real instrument's key range (timbre/register is clamped to it)
    float tcMidMs;            // hammer contact time at C4, hardness 0.5, full velocity
    float excScale;           // tine/reed excursion into the pickup nonlinearity
    float tA, tB;             // fundamental normal modes: tine-dominant (fast), tonebar-dominant (long)
    float t2, t3, t4;         // beam overtones
    float tDamp;              // damper felt
    float splitCents;         // split of the two fundamental normal modes (slow beating)
    float bell;               // overtone gain
    float ratio2Lo, ratio2Hi; // first overtone ratio at keyLo / keyHi (tuning spring / solder mass)
    float thumpHz, thumpT60, thumpAmp;
    float toneLo, toneHi;     // tone low-pass range (Hz)
    float presHz, presGain;   // presence / nasal peak
    float bassGain;           // low shelf (Suitcase preamp, Piano Bass)
    float hpHz;               // high-pass (Wurlitzer amp), 0 = off
    float bias, driveBase;    // preamp asymmetry and inherent drive
    float gain;               // output normalization
    float strikeLo, strikeHi; // strike point range along the tine/reed (0 = clamp, 1 = tip)
};

namespace {
using MD = EPiano::ModelDef;
// clang-format off
constexpr MD kModels[5] = {
    // Rhodes Mk I Stage: neoprene-tipped hammers, round tone
    {false, 28, 100, 1.7f, 1.00f, 1.5f, 7.0f, 0.40f, 0.12f, 0.05f, 0.10f, 1.4f, 1.0f, 7.6f, 5.2f,
     110.0f, 0.06f, 0.12f, 1200.0f, 9000.0f, 2500.0f, 0.00f, 0.15f, 0.0f, 0.15f, 0.00f, 0.30f, 0.18f, 0.60f},
    // Rhodes Mk II Stage: harder hammers, brighter, more bark
    {false, 28, 100, 1.3f, 1.15f, 1.3f, 6.5f, 0.45f, 0.14f, 0.06f, 0.10f, 1.4f, 1.2f, 7.6f, 5.2f,
     110.0f, 0.05f, 0.10f, 1600.0f, 12000.0f, 2500.0f, 0.35f, 0.05f, 0.0f, 0.15f, 0.00f, 0.36f, 0.18f, 0.60f},
    // Rhodes Suitcase: Mk I action, Peterson preamp (bass boost), stereo vibrato
    {false, 28, 100, 1.7f, 1.00f, 1.5f, 7.0f, 0.40f, 0.12f, 0.05f, 0.10f, 1.4f, 1.0f, 7.6f, 5.2f,
     110.0f, 0.06f, 0.12f, 1300.0f, 10000.0f, 3000.0f, 0.15f, 0.40f, 0.0f, 0.15f, 0.05f, 0.26f, 0.18f, 0.60f},
    // Wurlitzer 200A: steel reeds, electrostatic pickup, transistor preamp, small speaker
    {true, 33, 96, 1.5f, 1.00f, 0.9f, 3.2f, 0.25f, 0.08f, 0.03f, 0.07f, 0.4f, 1.1f, 5.9f, 5.3f,
     140.0f, 0.04f, 0.08f, 2000.0f, 9000.0f, 1100.0f, 0.60f, 0.00f, 110.0f, 0.35f, 0.25f, 0.42f, 0.30f, 0.80f},
    // Rhodes Piano Bass: 32 keys E1..B3, bass tines, felt hammers, dark, thumpy, shorter sustain
    {false, 28, 59, 2.6f, 1.10f, 0.6f, 2.4f, 0.25f, 0.08f, 0.04f, 0.12f, 1.0f, 0.6f, 7.8f, 7.0f,
     90.0f, 0.09f, 0.50f, 400.0f, 3500.0f, 2500.0f, 0.00f, 0.30f, 0.0f, 0.15f, 0.05f, 0.35f, 0.18f, 0.60f},
};
// clang-format on

inline float clamp01(float x) noexcept { return std::clamp(x, 0.0f, 1.0f); }
} // namespace

const ModuleInfo& EPiano::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "epiano";
        i.displayName = "Electric Piano";
        i.kind = ModuleKind::Instrument;
        i.category = "E.Piano";
        i.params = {
            enumParam("model", "Model",
                      {"Rhodes Mk I", "Rhodes Mk II", "Rhodes Suitcase", "Wurlitzer 200A", "Rhodes Piano Bass"}, 0,
                      "Model"),
            linearParam("tone", "Tone", 0.0f, 1.0f, 0.5f, {}, "Tone"),
            linearParam("bark", "Bark", 0.0f, 1.0f, 0.4f, {}, "Tone"),
            linearParam("hammer_hardness", "Hammer Hardness", 0.0f, 1.0f, 0.5f, {}, "Hammer"),
            linearParam("velocity_curve", "Velocity Curve", -1.0f, 1.0f, 0.0f, {}, "Hammer"),
            linearParam("dynamics", "Dynamic Range", 6.0f, 48.0f, 30.0f, "dB", "Hammer"),
            logParam("decay", "Decay", 0.25f, 4.0f, 1.0f, "x", "Envelope", 1.0f),
            logParam("release", "Release", 0.25f, 4.0f, 1.0f, "x", "Envelope", 1.0f),
            linearParam("tine_mix", "Tine / Tonebar", 0.0f, 1.0f, 0.5f, {}, "Tine"),
            linearParam("strike_position", "Strike Position", 0.0f, 1.0f, 0.5f, {}, "Tine"),
            linearParam("pickup_position", "Pickup Alignment", 0.0f, 1.0f, 0.45f, {}, "Pickup"),
            linearParam("pickup_distance", "Pickup Distance", 0.0f, 1.0f, 0.5f, {}, "Pickup"),
            linearParam("drive", "Preamp Drive", 0.0f, 1.0f, 0.1f, {}, "Amp"),
            linearParam("noise", "Mechanical Noise", 0.0f, 1.0f, 0.3f, {}, "Mechanics"),
            linearParam("key_variation", "Key Variation", 0.0f, 1.0f, 0.5f, {}, "Mechanics"),
            linearParam("stereo_depth", "Suitcase Vibrato Depth", 0.0f, 1.0f, 0.5f, {}, "Suitcase"),
            logParam("stereo_rate", "Suitcase Vibrato Rate", 0.5f, 10.0f, 4.5f, "Hz", "Suitcase", 3.0f),
            intParam("polyphony", "Polyphony", 1, kMaxVoices, kMaxVoices, {}, "Voice"),
            linearParam("volume_db", "Volume", -60.0f, 6.0f, -3.0f, "dB", "Amp"),
        };
        i.uiHints = {{"groupOrder", {"Model", "Tone", "Hammer", "Tine", "Pickup", "Envelope", "Amp", "Mechanics",
                                     "Suitcase", "Voice"}},
                     {"front", {"model", "tone", "bark", "hammer_hardness", "decay", "tine_mix", "drive",
                                "volume_db"}}};
        return i;
    }();
    return info;
}

float EPiano::keyRandom(int note, int salt) noexcept {
    uint32_t h = static_cast<uint32_t>(note) * 0x9E3779B1u ^ static_cast<uint32_t>(salt) * 0x85EBCA77u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 8388607.5f - 1.0f;
}

float EPiano::tuningCents(int note, float keyVariation) noexcept {
    const float c = static_cast<float>(note - 69);
    const float stretch = std::clamp(0.0018f * c * std::fabs(c), -6.0f, 6.0f); // mild octave stretch
    return stretch + 1.5f * clamp01(keyVariation) * keyRandom(note, 1);
}

float EPiano::pedalLiftFromCc(int cc) noexcept {
    // Half-damper: CC64 <= ~15 dampers fully on, >= ~96 fully lifted, smooth in between.
    const float x = clamp01((static_cast<float>(cc) / 127.0f - 0.12f) / 0.63f);
    return x * x * (3.0f - 2.0f * x);
}

EPiano::EPiano() : Module(moduleInfo()) {
    shared_.owner = this;
    shared_.model = &kModels[0];
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = alloc_.voice(i);
        v.sh = &shared_;
        v.index = i;
        v.rng = 0x9E3779B9u + static_cast<uint32_t>(i) * 7919u;
    }
    for (int i = 0; i < kGhosts; ++i) ghosts_[static_cast<size_t>(i)].sh = &shared_;
    alloc_.setSustainEnabled(false); // the pedal is handled as a continuous damper (half-pedal) here
}

void EPiano::prepare(double sampleRate, int /*maxBlock*/) {
    shared_.sampleRate = sampleRate;
    for (dsp::Svf* f : {&toneLp_, &presBp_, &bassLp_, &hp_}) f->setSampleRate(sampleRate);
    toneHz_ = presHz_ = hpHz_ = -1.0f;
    bassLp_.setCutoff(180.0f, 0.3f);
    dcR_ = 1.0f - dsp::kTwoPi * 10.0f / static_cast<float>(sampleRate);
    volume_.prepare(sampleRate, 0.02f);
    stereoDepth_.prepare(sampleRate, 0.05f);
    ProcessContext ctx;
    ctx.sampleRate = sampleRate;
    updateShared(ctx);
    reset();
}

void EPiano::reset() {
    alloc_.reset();
    for (auto& g : ghosts_) g.reset();
    for (dsp::Svf* f : {&toneLp_, &presBp_, &bassLp_, &hp_}) f->reset();
    dcX1_ = dcY1_ = 0.0f;
    lfoC_ = 1.0f;
    lfoS_ = 0.0f;
    pedalInit_ = false;
    pedalCc_ = 0;
    shared_.pedalLift = 0.0f;
    volume_.snap(dsp::dbToGain(params().get(VolumeDb)));
    stereoDepth_.snap(stereoDepth_.target());
    activeVoices_.store(0, std::memory_order_relaxed);
}

int EPiano::tailSamples() const { return static_cast<int>(2.0 * shared_.sampleRate); }

void EPiano::updateShared(const ProcessContext& ctx) noexcept {
    const ParamSet& p = params();
    const int m = std::clamp(static_cast<int>(p.get(Model)), 0, 4);
    shared_.modelId = static_cast<ModelId>(m);
    shared_.model = &kModels[m];
    shared_.tone = clamp01(p.get(Tone));
    shared_.bark = clamp01(p.get(Bark));
    shared_.hardness = clamp01(p.get(HammerHardness));
    shared_.velGamma = std::exp2(-std::clamp(p.get(VelocityCurve), -1.0f, 1.0f));
    shared_.dynamicsDb = std::clamp(p.get(Dynamics), 6.0f, 48.0f);
    shared_.decayMul = std::clamp(p.get(Decay), 0.25f, 4.0f);
    shared_.releaseMul = std::clamp(p.get(Release), 0.25f, 4.0f);
    shared_.tineMix = clamp01(p.get(TineMix));
    shared_.strike = clamp01(p.get(StrikePosition));
    shared_.pickupPos = clamp01(p.get(PickupPosition));
    shared_.pickupDist = clamp01(p.get(PickupDistance));
    shared_.noise = clamp01(p.get(Noise));
    shared_.keyVar = clamp01(p.get(KeyVariation));
    shared_.bendSemis = ctx.channel ? std::clamp(ctx.channel->pitchBend, -1.0f, 1.0f) * 2.0f : 0.0f;
    const double sr = shared_.sampleRate;
    shared_.engageCoef = 1.0f - static_cast<float>(std::exp(-kChunk / (0.006 * sr)));
    alloc_.setPolyphony(static_cast<int>(p.get(Polyphony)));

    // Post chain.
    const ModelDef& md = *shared_.model;
    const float toneHz = md.toneLo * std::pow(md.toneHi / md.toneLo, shared_.tone);
    if (toneHz != toneHz_) {
        toneHz_ = toneHz;
        toneLp_.setCutoff(toneHz, 0.3f);
    }
    if (md.presHz != presHz_) {
        presHz_ = md.presHz;
        presBp_.setCutoff(md.presHz, 0.59f);
    }
    presGain_ = md.presGain * (0.5f + shared_.tone);
    bassGain_ = md.bassGain;
    useHp_ = md.hpHz > 0.0f;
    if (useHp_ && md.hpHz != hpHz_) {
        hpHz_ = md.hpHz;
        hp_.setCutoff(md.hpHz, 0.3f);
    }
    const float drive = std::clamp(md.driveBase + clamp01(p.get(Drive)), 0.0f, 1.25f);
    preDrive_ = 0.6f + 6.0f * drive;
    preBias_ = md.bias;
    const float tb = std::tanh(preBias_);
    preNorm_ = 1.0f / (preDrive_ * (1.0f - tb * tb));
    stereoDepth_.setTarget(shared_.modelId == ModelId::Suitcase ? clamp01(p.get(StereoDepth)) : 0.0f);
    volume_.setTarget(dsp::dbToGain(p.get(VolumeDb)));
}

// ---------------------------------------------------------------------------------------------------------------
// Voice

void EPiano::Voice::reset() noexcept {
    modes.reset();
    active = held = forceDamp = damperWasOn = false;
    note = -1;
    pulsePos = pulseLen = 0;
    ctlCountdown = 0;
    engage = 0.0f;
    engageApplied = -1.0f;
    prevPhi = 0.0f;
    clickEnv = thudEnv = noiseHp = thudLp = 0.0f;
    fade = 1.0f;
    fadeStep = 0.0f;
}

void EPiano::Voice::kill() noexcept {
    if (active && sh && sh->owner) sh->owner->adoptGhost(*this);
    reset();
}

void EPiano::Voice::noteOn(const VoiceStart& s) noexcept {
    const bool restrike = active && note == s.note;
    note = s.note;
    velocity = clamp01(s.velocity);
    held = true;
    forceDamp = false;
    setupNote(restrike);
    active = true;
}

void EPiano::Voice::setupNote(bool restrike) noexcept {
    const Shared& S = *sh;
    const ModelDef& M = *S.model;
    const double sr = S.sampleRate;
    const float srf = static_cast<float>(sr);
    electrostatic = M.electrostatic;

    const int nc = std::clamp(note, M.keyLo, M.keyHi);
    const float t = static_cast<float>(nc - M.keyLo) / static_cast<float>(M.keyHi - M.keyLo);
    const float reg = static_cast<float>(nc - 60);
    const float kv = S.keyVar;

    f0 = dsp::noteToHz(static_cast<float>(note) + tuningCents(note, kv) * 0.01f);
    const float v = std::pow(velocity, S.velGamma);
    const float exc = dsp::dbToGain(-S.dynamicsDb * (1.0f - v));

    // Hammer: alpha force pulse, contact time tc ~ 4 tau. Softer hammer / lower velocity / bass = longer contact
    // = fewer highs (Hertz-like: stiffer felt compression at higher speed).
    float tc = M.tcMidMs * 1e-3f * std::exp2(-reg / 36.0f) * std::exp2(1.0f - 2.0f * S.hardness) *
               std::pow(0.15f + v, -0.4f) * 1.0575f; // 1.0575 = 1.15^0.4 (unity at full velocity)
    // A regulated hammer leaves within ~half a period: smooth (monotonic) limit towards 0.6 / f0.
    const float tcMax = 0.6f / f0;
    tc = tc / std::sqrt(1.0f + (tc / tcMax) * (tc / tcMax));
    tc = std::clamp(tc, 2.0f / srf, 0.008f);
    const float tauS = 0.25f * tc * srf; // tau in samples (>= 0.5)
    pulseLen = static_cast<int>(std::ceil(8.0f * tauS));
    pulsePos = 0;
    pulseInvTau = 1.0f / tauS;
    pulseDecay = std::exp(-pulseInvTau);
    pulseEnv = std::exp(1.0f - 0.5f * pulseInvTau);
    const float pulseSum = 2.718281828f * tauS; // integral of the pulse in samples
    const float gNorm = 1.0f / (pulseSum * dsp::alphaPulseSpectrum(f0, 0.25f * tc));

    // Excursion into the pickup.
    const float barkScale = 0.25f + 2.5f * std::pow(S.bark, 1.4f);
    const float barkReg = std::exp2(-reg / 36.0f) * M.excScale;
    const float X = exc * barkScale * barkReg;

    // Modes.
    const double xi = M.strikeLo + (M.strikeHi - M.strikeLo) * S.strike;
    const float split = M.splitCents * (1.0f + 0.6f * kv * keyRandom(note, 4));
    const float ratio2 = (M.ratio2Lo + (M.ratio2Hi - M.ratio2Lo) * t) * (1.0f + 0.02f * kv * keyRandom(note, 5));
    const float pol = 3.0f + 4.0f * kv * std::fabs(keyRandom(note, 7)); // second tine polarization (cents)
    const float bellGain = M.bell * (0.5f + S.tone);
    const float b2 = static_cast<float>(dsp::beam::struckTipAmplitude(1, xi)) * 6.267f / ratio2 * bellGain;
    const float b3 = static_cast<float>(dsp::beam::struckTipAmplitude(2, xi)) * bellGain;
    const float b4 = static_cast<float>(dsp::beam::struckTipAmplitude(3, xi)) * bellGain;
    const float D = std::exp2(-reg / 22.0f) * (1.0f + 0.12f * kv * keyRandom(note, 3));
    const float sD = std::sqrt(D);
    const float dm = S.decayMul, sdm = std::sqrt(dm);
    float tDamp = M.tDamp * std::exp2(-reg / 30.0f) * S.releaseMul;
    if (!M.electrostatic && S.modelId != ModelId::PianoBass && note > 96) tDamp *= 5.0f; // top keys: no dampers
    const float thumpLevel = M.thumpAmp * 3.0f * S.noise; // key/hammer thump, part of the mechanical noise

    struct ModeSpec {
        float ratio, amp, t60, busA, busB;
    };
    const ModeSpec spec[kModes] = {
        {std::exp2(-split / 2400.0f), S.tineMix, M.tA * D * dm, 1.0f, 0.0f},
        {std::exp2(split / 2400.0f), 1.0f - S.tineMix, std::min(M.tB * D * dm, 30.0f), 1.0f, 0.0f},
        {ratio2, 0.5f * b2, M.t2 * sD * sdm, 1.0f, 0.0f},
        {ratio2 * std::exp2(pol / 1200.0f), 0.5f * b2, M.t2 * sD * sdm * 0.8f, 1.0f, 0.0f},
        {ratio2 * 2.7998f, b3, M.t3 * sD * sdm, 1.0f, 0.0f},
        {ratio2 * 5.4869f, b4, M.t4 * sD * sdm, 1.0f, 0.0f},
        {0.0f, thumpLevel, M.thumpT60, 0.0f, 1.0f}, // thump: absolute frequency below
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},             // spare
    };
    const float bendRatio = std::exp2(S.bendSemis / 12.0f);
    for (int k = 0; k < kModes; ++k) {
        const ModeSpec& ms = spec[k];
        const size_t kk = static_cast<size_t>(k);
        const float f = k == kThumpMode ? M.thumpHz * (1.0f + 0.3f * t) : f0 * ms.ratio;
        const float omega = dsp::kTwoPi * f / srf;
        if (ms.amp == 0.0f || ms.t60 <= 0.0f || f <= 0.0f || omega * (k == kThumpMode ? 1.0f : bendRatio) > 0.9f * dsp::kPi) {
            if (restrike && baseOmega[kk] > 0.0f) { // still ringing: keep it, just don't excite it again
                modes.setInputGain(k, 0.0f);
                continue;
            }
            modes.setMode(k, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            baseOmega[kk] = 0.0f;
            rateFree[kk] = rateDamp[kk] = 0.0f;
            continue;
        }
        baseOmega[kk] = omega;
        rateFree[kk] = kLn1000 / ms.t60;
        rateDamp[kk] = std::max(rateFree[kk], kLn1000 / std::min(ms.t60, tDamp));
        // Thump is acoustic (bus B, absolute level ~ exc); tine/reed modes scale with the excursion X.
        const float g = ms.amp * (k == kThumpMode ? exc : X) * gNorm;
        const float w = k == kThumpMode ? omega : omega * bendRatio;
        modes.setMode(k, w, dsp::ModalBank<kModes>::radiusForT60(ms.t60, sr), g, ms.busA, ms.busB);
    }
    engageApplied = -1.0f; // re-apply damping on the next control update
    bendApplied = S.bendSemis;

    // Pickup.
    const float hzNorm = srf / (dsp::kTwoPi * f0); // derivative pickup -> amplitude independent of pitch
    const float bScale = barkScale * barkReg;
    if (!M.electrostatic) {
        const float w = 0.55f + 1.3f * S.pickupDist;
        invW = 1.0f / w;
        u0 = 0.05f + 0.9f * S.pickupPos + 0.08f * kv * keyRandom(note, 6);
        const float q = 1.0f + u0 * u0;
        const float slope = 2.0f * std::fabs(u0) / (q * q);
        pickupNorm = hzNorm * w / (std::max(slope, 0.25f) * bScale);
        uBuzz = 1.7f + 1.6f * S.pickupDist;
        // closer pickup = stronger field = louder
        outGain = M.gain * (1.25f - 0.5f * S.pickupDist);
    } else {
        const float gap = 0.7f + 1.0f * S.pickupDist;
        invW = 1.0f / gap;
        nlA = 0.25f + 0.55f * S.pickupPos;
        pickupNorm = hzNorm * gap / bScale;
        uBuzz = 1e9f;
        outGain = M.gain * (1.25f - 0.5f * S.pickupDist);
    }
    // Pickup state from the current displacement with the (possibly changed) geometry: no step on re-strike.
    prevPhi = pickupCurve(modes.busA());
    outGain *= std::exp2(-reg / 80.0f) * (1.0f + 0.12f * kv * keyRandom(note, 2));
    silenceScale = outGain * outGain / (bScale * bScale);

    // Mechanical noise.
    clickLevel = S.noise * 0.05f * exc;
    clickEnv = 1.0f;
    clickDecay = static_cast<float>(std::exp(-1.0 / (0.0025 * sr)));
    thudLevel = S.noise * 0.04f * (0.3f + velocity);
    thudDecay = static_cast<float>(std::exp(-1.0 / (0.02 * sr)));
    thudLpCoef = 1.0f - static_cast<float>(std::exp(-dsp::kTwoPi * 600.0 / sr));
    buzzLevel = 0.1f + 0.4f * S.noise;
    damperWasOn = false;
    ctlCountdown = 0; // apply damping/bend before the first sample
    if (!restrike) {
        engage = 0.0f;
        fade = 1.0f;
        fadeStep = 0.0f;
    }
}

void EPiano::Voice::updateControl() noexcept {
    const Shared& S = *sh;
    const float target = forceDamp ? 1.0f : (held ? 0.0f : 1.0f - S.pedalLift);
    const bool damperOn = target > 0.5f;
    if (damperOn && !damperWasOn) thudEnv = 1.0f; // felt lands on the tine
    damperWasOn = damperOn;

    engage += (target - engage) * S.engageCoef;
    if (std::fabs(target - engage) < 1e-3f) engage = target;
    if (engage != engageApplied) {
        const float e2 = engage * engage; // felt contact area grows quickly once touching
        const double sr = S.sampleRate;
        for (int k = 0; k < kModes; ++k) {
            const size_t kk = static_cast<size_t>(k);
            if (baseOmega[kk] <= 0.0f) continue;
            const float rate = rateFree[kk] + e2 * (rateDamp[kk] - rateFree[kk]);
            modes.setRadius(k, static_cast<float>(std::exp(-static_cast<double>(rate) / sr)));
        }
        engageApplied = engage;
    }
    if (S.bendSemis != bendApplied) {
        const float ratio = std::exp2(S.bendSemis / 12.0f);
        for (int k = 0; k < kModes; ++k) {
            const size_t kk = static_cast<size_t>(k);
            if (k == kThumpMode || baseOmega[kk] <= 0.0f) continue;
            modes.setFrequency(k, std::min(baseOmega[kk] * ratio, 0.95f * dsp::kPi));
        }
        bendApplied = S.bendSemis;
    }
}

float EPiano::Voice::pickupCurve(float a) const noexcept {
    if (!electrostatic) {
        float u = a * invW;
        if (u > uBuzz) u = uBuzz + (u - uBuzz) * 0.25f; // tine slaps towards the pickup: soft contact limit
        u += u0;
        return 1.0f / (1.0f + u * u);
    }
    // C ~ 1/(gap - x): steeper as the reed swings towards the plate. The sigmoid in the denominator keeps it
    // bounded (>= 1 - a) without clipping the waveform.
    const float x = a * invW;
    return x / (1.0f - nlA * x / std::sqrt(1.0f + x * x));
}

void EPiano::Voice::render(float* out, int n) noexcept {
    int i = 0;
    while (i < n) {
        if (ctlCountdown <= 0) { // control rate: every kChunk samples, independent of block segmentation
            if (pulsePos >= pulseLen && clickEnv <= 1e-4f && thudEnv <= 1e-4f &&
                modes.energy() * silenceScale < kSilence) {
                reset();
                return;
            }
            updateControl();
            ctlCountdown = kChunk;
        }
        const int m = std::min(ctlCountdown, n - i);
        float* o = out + i;
        for (int j = 0; j < m; ++j) {
            float xin = 0.0f;
            if (pulsePos < pulseLen) {
                xin = (static_cast<float>(pulsePos) + 0.5f) * pulseInvTau * pulseEnv;
                pulseEnv *= pulseDecay;
                ++pulsePos;
            }
            const auto r = modes.tick(xin);
            const float phi = pickupCurve(r.a);
            float y = (phi - prevPhi) * pickupNorm + r.b;
            prevPhi = phi;
            if (!electrostatic) {
                const float ex = r.a * invW - uBuzz;
                if (ex > 0.0f) y += std::min(ex, 2.0f) * buzzLevel * 0.05f * whiteNoise(); // contact rattle
            }
            if (clickEnv > 1e-4f) {
                const float w = whiteNoise();
                y += (w - noiseHp) * clickEnv * clickLevel;
                noiseHp = w;
                clickEnv *= clickDecay;
            }
            if (thudEnv > 1e-4f) {
                thudLp += (whiteNoise() - thudLp) * thudLpCoef;
                y += thudLp * thudEnv * thudLevel;
                thudEnv *= thudDecay;
            }
            o[j] += y * outGain * fade;
            if (fadeStep > 0.0f) {
                fade -= fadeStep;
                if (fade <= 0.0f) {
                    reset();
                    return;
                }
            }
        }
        ctlCountdown -= m;
        i += m;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Module

void EPiano::adoptGhost(const Voice& v) noexcept {
    size_t best = 0;
    float bestFade = 2.0f;
    for (size_t i = 0; i < ghosts_.size(); ++i) {
        if (!ghosts_[i].active) {
            best = i;
            bestFade = -1.0f;
            break;
        }
        if (ghosts_[i].fade < bestFade) {
            bestFade = ghosts_[i].fade;
            best = i;
        }
    }
    Voice& g = ghosts_[best];
    const uint32_t rng = g.rng;
    g = v;
    g.rng = rng ^ 0xA5A5A5A5u;
    if (g.rng == 0) g.rng = 1;
    g.fadeStep = std::min(1.0f, 1.0f / static_cast<float>(0.006 * shared_.sampleRate));
}

void EPiano::renderSegment(int start, int end) noexcept {
    if (end <= start) return;
    float* o = mono_ + start;
    const int n = end - start;
    alloc_.forEachActive([&](Voice& v) { v.render(o, n); });
    for (auto& g : ghosts_)
        if (g.active) g.render(o, n);
}

void EPiano::postProcess(AudioBlock& out) noexcept {
    const int n = out.numSamples;
    const float tb = std::tanh(preBias_);
    const float bpk = 2.0f - 1.98f * 0.59f; // presence band-pass normalization (peak gain 1)
    const bool stereo = stereoDepth_.target() > 0.0f || stereoDepth_.value() > 0.0f;
    float cr = 1.0f, sr = 0.0f;
    if (stereo) {
        const float w = dsp::kTwoPi * std::clamp(params().get(StereoRate), 0.5f, 10.0f) /
                        static_cast<float>(shared_.sampleRate);
        cr = std::cos(w);
        sr = std::sin(w);
    }
    for (int i = 0; i < n; ++i) {
        float x = out.left[i];
        x = (std::tanh(preDrive_ * x + preBias_) - tb) * preNorm_;
        const float y = x - dcX1_ + dcR_ * dcY1_;
        dcX1_ = x;
        dcY1_ = y;
        float t = toneLp_.tick(y).lp;
        if (presGain_ != 0.0f) t += presGain_ * bpk * presBp_.tick(t).bp;
        if (bassGain_ != 0.0f) t += bassGain_ * bassLp_.tick(t).lp;
        if (useHp_) t = hp_.tick(t).hp;
        t *= volume_.next();
        if (stereo) {
            const float c = lfoC_ * cr - lfoS_ * sr;
            lfoS_ = lfoS_ * cr + lfoC_ * sr;
            lfoC_ = c;
            const float p = stereoDepth_.next() * lfoS_;
            out.left[i] = t * std::sqrt(std::max(0.0f, 1.0f - p));
            out.right[i] = t * std::sqrt(std::max(0.0f, 1.0f + p));
        } else {
            out.left[i] = t;
            out.right[i] = t;
        }
    }
    if (stereo) { // keep the quadrature oscillator on the unit circle
        const float g = 1.5f - 0.5f * (lfoC_ * lfoC_ + lfoS_ * lfoS_);
        lfoC_ *= g;
        lfoS_ *= g;
    }
}

void EPiano::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    if (!pedalInit_) {
        pedalCc_ = ctx.channel && ctx.channel->sustain ? 127 : 0;
        shared_.pedalLift = pedalLiftFromCc(pedalCc_);
        pedalInit_ = true;
    }
    updateShared(ctx);
    mono_ = out.left;
    int pos = 0;
    for (const MidiEvent& e : events) {
        const int at = std::clamp(static_cast<int>(e.sampleOffset), pos, out.numSamples);
        renderSegment(pos, at);
        pos = at;
        if (e.type == MidiEventType::ControlChange && (e.data1 == 64 || e.data1 == 121)) {
            pedalCc_ = e.data1 == 64 ? e.value7 : 0; // CC121 = reset all controllers
            shared_.pedalLift = pedalLiftFromCc(pedalCc_);
            continue;
        }
        alloc_.handleEvent(e);
        if (e.type == MidiEventType::AllNotesOff) {
            for (int i = 0; i < kMaxVoices; ++i) alloc_.voice(i).forceDamp = true;
        } else if (e.type == MidiEventType::AllSoundOff) {
            for (auto& g : ghosts_) g.reset();
        }
    }
    renderSegment(pos, out.numSamples);
    postProcess(out);
    int count = alloc_.activeCount();
    for (const auto& g : ghosts_) count += g.active ? 1 : 0;
    activeVoices_.store(count, std::memory_order_relaxed);
}

} // namespace ks
