#include "instruments/va/VaSynth.h"

#include "dsp/FastMath.h"
#include "dsp/Lfo.h"
#include "dsp/Math.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ks {

namespace {

constexpr float kVoiceGain = 1.2f; // per-note output scale (headroom for chords)

template <typename T>
T clampv(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

int toInt(float v) noexcept { return static_cast<int>(std::lround(v)); }

const std::vector<std::string>& modSourceLabels() {
    static const std::vector<std::string> v = {"Off",      "LFO1",       "LFO2",       "Filter Env",
                                               "Amp Env",  "Velocity",   "Key",        "Mod Wheel",
                                               "Aftertouch", "Pitch Bend", "Expression", "Random"};
    return v;
}

const std::vector<std::string>& modDestLabels() {
    static const std::vector<std::string> v = {
        "Off",         "Pitch",       "Osc1 Pitch", "Osc2 Pitch", "Osc3 Pitch", "Osc1 PW",   "Osc2 PW",
        "Osc3 PW",     "Osc1 Level",  "Osc2 Level", "Osc3 Level", "Sub Level",  "Noise Level", "FM Amount",
        "Cutoff",      "Resonance",   "HPF",        "Amp",        "Pan",        "LFO1 Depth", "Supersaw Detune",
        "Filter Env Amt"};
    return v;
}

// Full-scale of a mod amount of 1.0 per destination (plain units: semitones, octaves, PW, levels).
float modDestScale(int dst) noexcept {
    switch (dst) {
    case VaSynth::DstPitch:
    case VaSynth::DstOsc1Pitch:
    case VaSynth::DstOsc2Pitch:
    case VaSynth::DstOsc3Pitch: return 24.0f;
    case VaSynth::DstOsc1Pw:
    case VaSynth::DstOsc2Pw:
    case VaSynth::DstOsc3Pw: return 0.45f;
    case VaSynth::DstCutoff:
    case VaSynth::DstHpf:
    case VaSynth::DstFenvAmt: return 8.0f;
    default: return 1.0f;
    }
}

// Alternating voice pan positions for pan spread: -1, +1, -0.75, +0.75, ...
float voicePanPattern(int index) noexcept {
    const float sign = (index & 1) ? 1.0f : -1.0f;
    const float mag = 1.0f - 0.25f * static_cast<float>((index >> 1) & 3);
    return sign * mag;
}

} // namespace

const ModuleInfo& VaSynth::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "va";
        i.displayName = "Virtual Analog";
        i.kind = ModuleKind::Instrument;
        i.category = "Synth";
        const std::vector<std::string> waves = {"Saw", "Pulse", "Triangle", "Sine", "Supersaw", "Noise"};
        auto& ps = i.params;
        auto oscBlock = [&](int n, float defLevel) {
            const std::string p = "osc" + std::to_string(n) + "_";
            const std::string g = "Osc" + std::to_string(n);
            ps.push_back(enumParam(p + "wave", "Wave", waves, 0, g));
            ps.push_back(intParam(p + "octave", "Octave", -3, 3, 0, "oct", g));
            ps.push_back(intParam(p + "semi", "Semitone", -12, 12, 0, "st", g));
            ps.push_back(linearParam(p + "fine", "Fine", -100.0f, 100.0f, 0.0f, "ct", g));
            ps.push_back(linearParam(p + "level", "Level", 0.0f, 1.0f, defLevel, {}, g));
            ps.push_back(linearParam(p + "pw", "PW / Mix", 0.05f, 0.95f, 0.5f, {}, g));
            ps.push_back(linearParam(p + "pwm", "PWM (LFO1)", 0.0f, 1.0f, 0.0f, {}, g));
            ps.push_back(linearParam(p + "detune", "Supersaw Detune", 0.0f, 1.0f, 0.5f, {}, g));
        };
        oscBlock(1, 1.0f);
        ps.push_back(boolParam("osc1_sync", "Sync to Osc2", false, "Osc1"));
        ps.push_back(linearParam("osc1_fm", "FM from Osc3", 0.0f, 1.0f, 0.0f, {}, "Osc1"));
        ps.push_back(linearParam("osc1_pm_env", "Filter Env > Pitch", -48.0f, 48.0f, 0.0f, "st", "Osc1"));
        oscBlock(2, 0.0f);
        oscBlock(3, 0.0f);
        ps.push_back(linearParam("sub_level", "Sub Level", 0.0f, 1.0f, 0.0f, {}, "Sub/Noise"));
        ps.push_back(enumParam("sub_octave", "Sub Octave", {"-1 Oct", "-2 Oct"}, 0, "Sub/Noise"));
        ps.push_back(linearParam("noise_level", "Noise Level", 0.0f, 1.0f, 0.0f, {}, "Sub/Noise"));
        ps.push_back(linearParam("ring_level", "Ring Mod (1x2)", 0.0f, 1.0f, 0.0f, {}, "Mixer"));
        ps.push_back(enumParam("osc_reset", "Osc Phase", {"Free Run", "Reset"}, 0, "Mixer"));
        ps.push_back(enumParam("filter_model", "Model", {"Ladder 24 (Moog)", "IR3109 24 (Juno)", "SEM 12 (OB)"}, 0,
                               "Filter"));
        ps.push_back(enumParam("filter_mode", "Mode", {"Low Pass", "Band Pass", "High Pass", "Notch"}, 0, "Filter"));
        ps.push_back(logParam("cutoff", "Cutoff", 20.0f, 20000.0f, 8000.0f, "Hz", "Filter", 1000.0f));
        ps.push_back(linearParam("resonance", "Resonance", 0.0f, 1.0f, 0.0f, {}, "Filter"));
        ps.push_back(linearParam("filter_drive", "Drive", 0.0f, 1.0f, 0.0f, {}, "Filter"));
        ps.push_back(linearParam("key_track", "Key Track", 0.0f, 1.5f, 0.5f, "x", "Filter"));
        ps.push_back(linearParam("filter_env_amt", "Env Amount", -8.0f, 8.0f, 0.0f, "oct", "Filter"));
        ps.push_back(linearParam("filter_vel", "Velocity > Cutoff", 0.0f, 4.0f, 0.0f, "oct", "Filter"));
        ps.push_back(logParam("hpf_cutoff", "HPF", 10.0f, 4000.0f, 10.0f, "Hz", "Filter", 200.0f));
        ps.push_back(logParam("fenv_attack", "Attack", 0.0005f, 10.0f, 0.005f, "s", "Filter Env", 0.1f));
        ps.push_back(logParam("fenv_decay", "Decay", 0.001f, 20.0f, 0.5f, "s", "Filter Env", 0.5f));
        ps.push_back(linearParam("fenv_sustain", "Sustain", 0.0f, 1.0f, 0.5f, {}, "Filter Env"));
        ps.push_back(logParam("fenv_release", "Release", 0.001f, 20.0f, 0.5f, "s", "Filter Env", 0.5f));
        ps.push_back(linearParam("fenv_vel", "Velocity", 0.0f, 1.0f, 0.0f, {}, "Filter Env"));
        ps.push_back(logParam("amp_attack", "Attack", 0.0005f, 10.0f, 0.002f, "s", "Amp Env", 0.1f));
        ps.push_back(logParam("amp_decay", "Decay", 0.001f, 20.0f, 0.5f, "s", "Amp Env", 0.5f));
        ps.push_back(linearParam("amp_sustain", "Sustain", 0.0f, 1.0f, 1.0f, {}, "Amp Env"));
        ps.push_back(logParam("amp_release", "Release", 0.001f, 20.0f, 0.3f, "s", "Amp Env", 0.5f));
        ps.push_back(linearParam("amp_vel", "Velocity", 0.0f, 1.0f, 0.5f, {}, "Amp Env"));
        const std::vector<std::string> lfoWaves = {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H"};
        const std::vector<std::string> divisions(dsp::kLfoDivisionLabels,
                                                 dsp::kLfoDivisionLabels + dsp::kLfoDivisionCount);
        auto lfoBlock = [&](int n) {
            const std::string p = "lfo" + std::to_string(n) + "_";
            const std::string g = "LFO" + std::to_string(n);
            ps.push_back(enumParam(p + "wave", "Wave", lfoWaves, 1, g));
            ps.push_back(logParam(p + "rate", "Rate", 0.01f, 50.0f, 5.0f, "Hz", g, 2.0f));
            ps.push_back(boolParam(p + "sync", "Tempo Sync", false, g));
            ps.push_back(enumParam(p + "division", "Division", divisions, 8, g));
            ps.push_back(linearParam(p + "delay", "Delay", 0.0f, 5.0f, 0.0f, "s", g));
            ps.push_back(boolParam(p + "key_sync", "Key Sync", false, g));
        };
        lfoBlock(1);
        ps.push_back(linearParam("lfo1_pitch", "Vibrato", 0.0f, 12.0f, 0.0f, "st", "LFO1"));
        ps.push_back(linearParam("lfo1_cutoff", "> Cutoff", 0.0f, 4.0f, 0.0f, "oct", "LFO1"));
        ps.push_back(linearParam("lfo1_amp", "Tremolo", 0.0f, 1.0f, 0.0f, {}, "LFO1"));
        lfoBlock(2);
        for (int s = 1; s <= kModSlots; ++s) {
            const std::string p = "mod" + std::to_string(s) + "_";
            const std::string n = "Mod " + std::to_string(s) + " ";
            ps.push_back(enumParam(p + "src", n + "Source", modSourceLabels(), 0, "Mod Matrix"));
            ps.push_back(enumParam(p + "dst", n + "Dest", modDestLabels(), 0, "Mod Matrix"));
            ps.push_back(linearParam(p + "amt", n + "Amount", -1.0f, 1.0f, 0.0f, {}, "Mod Matrix"));
        }
        ps.push_back(enumParam("voice_mode", "Voice Mode", {"Poly", "Mono", "Legato"}, 0, "Voice"));
        ps.push_back(intParam("polyphony", "Polyphony", 1, kMaxVoices, kMaxVoices, {}, "Voice"));
        ps.push_back(intParam("unison", "Unison", 1, kMaxUnison, 1, {}, "Voice"));
        ps.push_back(linearParam("unison_detune", "Unison Detune", 0.0f, 100.0f, 15.0f, "ct", "Voice"));
        ps.push_back(linearParam("unison_spread", "Unison Spread", 0.0f, 1.0f, 0.5f, {}, "Voice"));
        ps.push_back(linearParam("glide", "Glide", 0.0f, 5.0f, 0.0f, "s", "Voice"));
        ps.push_back(enumParam("glide_mode", "Glide Mode", {"Always", "Legato Only"}, 0, "Voice"));
        ps.push_back(linearParam("drift", "Analog Drift", 0.0f, 1.0f, 0.2f, {}, "Voice"));
        ps.push_back(linearParam("pan_spread", "Voice Pan Spread", 0.0f, 1.0f, 0.0f, {}, "Voice"));
        ps.push_back(intParam("bend_range", "Bend Range", 0, 24, 2, "st", "Voice"));
        ps.push_back(linearParam("mw_vibrato", "Wheel > Vibrato (LFO2)", 0.0f, 2.0f, 0.5f, "st", "Voice"));
        ps.push_back(linearParam("mw_cutoff", "Wheel > Cutoff (LFO2)", 0.0f, 4.0f, 0.0f, "oct", "Voice"));
        ps.push_back(linearParam("at_vibrato", "Aftertouch > Vibrato", 0.0f, 2.0f, 0.0f, "st", "Voice"));
        ps.push_back(linearParam("at_cutoff", "Aftertouch > Cutoff", -4.0f, 4.0f, 0.0f, "oct", "Voice"));
        ps.push_back(linearParam("drive", "Drive", 0.0f, 1.0f, 0.0f, {}, "Output"));
        ps.push_back(linearParam("volume_db", "Volume", -60.0f, 6.0f, -6.0f, "dB", "Output"));

        i.uiHints = {{"groupOrder",
                      {"Osc1", "Osc2", "Osc3", "Sub/Noise", "Mixer", "Filter", "Filter Env", "Amp Env", "LFO1", "LFO2",
                       "Mod Matrix", "Voice", "Output"}},
                     {"front",
                      {"osc1_wave", "osc2_wave", "osc2_fine", "cutoff", "resonance", "filter_env_amt", "amp_attack",
                       "amp_release", "unison", "glide", "volume_db"}}};
        return i;
    }();
    return info;
}

VaSynth::VaSynth() : Module(moduleInfo()) {
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = alloc_.voice(i);
        v.shared = &shared_;
        v.index = i;
    }
}

void VaSynth::prepare(double sampleRate, int /*maxBlock*/) {
    shared_.sampleRate = sampleRate;
    shared_.sr = static_cast<float>(sampleRate);
    shared_.invSr = 1.0f / shared_.sr;
    shared_.tickSeconds = static_cast<float>(kControlInterval) / shared_.sr;
    const double tickRate = sampleRate / kControlInterval;
    shared_.smCoef = dsp::onePoleCoef(0.008f, tickRate);
    shared_.driftCoef = dsp::onePoleCoef(0.4f, tickRate);
    shared_.driftTickScale = static_cast<float>(sampleRate / 48000.0);
    dsp::WhiteNoise rng(0xC0FFEEu);
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = alloc_.voice(i);
        v.seed = dsp::hash32(static_cast<uint32_t>(i) + 101u);
        for (int s = 0; s < kMaxUnison; ++s) {
            auto& sv = v.subs[static_cast<size_t>(s)];
            sv.svf.setSampleRate(sampleRate);
            sv.seed = dsp::hash32(v.seed + static_cast<uint32_t>(s) * 7919u);
            sv.noise.setSeed(sv.seed | 1u);
            sv.driftStatic = rng.next();
            sv.driftCutStatic = rng.next();
            sv.driftValue = sv.driftTarget = rng.next();
            sv.driftCut = sv.driftCutTarget = rng.next();
            for (int k = 0; k < kNumOscs; ++k) {
                sv.osc[k].reset(rng.nextUnipolar());
                sv.saw[k].reset(sv.seed + static_cast<unsigned>(k));
            }
            sv.sub.reset(rng.nextUnipolar());
        }
    }
    volume_.prepare(sampleRate, 0.02f);
    drive_.prepare(sampleRate, 0.02f);
    dcCoef_ = 1.0f - dsp::kTwoPi * 5.0f / shared_.sr;
    ProcessContext ctx;
    ctx.sampleRate = sampleRate;
    updateShared(ctx);
    volume_.snap(dsp::dbToGain(params().get(VolumeDb)));
    drive_.snap(params().get(Drive));
    reset();
}

void VaSynth::reset() {
    alloc_.reset();
    keyDown_.fill(false);
    keysDown_ = 0;
    dcL_ = dcR_ = 0.0f;
    activeSubVoices_.store(0, std::memory_order_relaxed);
}

int VaSynth::tailSamples() const {
    return static_cast<int>(params().get(AmpRelease) * static_cast<float>(shared_.sampleRate)) + 512;
}

int VaSynth::activeSubVoices() const noexcept {
    int n = 0;
    for (int i = 0; i < kMaxVoices; ++i) {
        const auto& v = alloc_.voice(i);
        if (v.isActive()) n += v.numSubs;
    }
    return n;
}

void VaSynth::updateShared(const ProcessContext& ctx) noexcept {
    const ParamSet& p = params();
    Shared& s = shared_;
    for (int k = 0; k < kNumOscs; ++k) {
        const int base = k == 0 ? Osc1Wave : (k == 1 ? Osc2Wave : Osc3Wave);
        OscShared& o = s.osc[k];
        const auto type = static_cast<OscType>(clampv(toInt(p.get(base + 0)), 0, 5));
        if (type != o.type) ++s.typeEpoch;
        o.type = type;
        o.pitchOffset = 12.0f * p.get(base + 1) + p.get(base + 2) + 0.01f * p.get(base + 3);
        o.level = p.get(base + 4);
        o.pw = p.get(base + 5);
        o.pwm = p.get(base + 6);
        o.detune = p.get(base + 7);
        s.ssCurve[k] = o.type == OscType::SuperSaw ? dsp::SuperSaw::detuneCurve(o.detune) : 0.0f;
        s.smTarget[SmLevel1 + k] = o.level;
        s.smTarget[SmPw1 + k] = o.pw;
    }
    s.sync = p.get(Osc1Sync) >= 0.5f;
    s.fm = p.get(Osc1Fm);
    s.pmEnv = p.get(Osc1PmEnv);
    s.subLevel = p.get(SubLevel);
    s.subDiv = toInt(p.get(SubOctave)) == 0 ? 2 : 4;
    s.noiseLevel = p.get(NoiseLevel);
    s.ringLevel = p.get(RingLevel);
    s.phaseReset = toInt(p.get(OscReset)) == 1;

    s.filter = static_cast<FilterType>(clampv(toInt(p.get(FilterModel)), 0, 2));
    s.filterMode = clampv(toInt(p.get(FilterMode)), 0, 3);
    s.cutoffOct = std::log2(std::fmax(p.get(Cutoff), 1.0f));
    s.resonance = p.get(Resonance);
    const float drive = p.get(FilterDrive);
    s.driveGain = dsp::dbToGain(24.0f * drive);
    s.driveComp = 1.0f / std::sqrt(s.driveGain);
    s.keyTrack = p.get(KeyTrack);
    s.fenvAmt = p.get(FilterEnvAmt);
    s.filterVel = p.get(FilterVel);
    s.hpfOct = std::log2(std::fmax(p.get(HpfCutoff), 1.0f));
    s.smTarget[SmCutoff] = s.cutoffOct;
    s.smTarget[SmResonance] = s.resonance;
    s.smTarget[SmDrive] = drive;
    s.smTarget[SmSub] = s.subLevel;
    s.smTarget[SmNoise] = s.noiseLevel;
    s.smTarget[SmRing] = s.ringLevel;
    s.smTarget[SmFm] = s.fm;
    s.smTarget[SmHpf] = s.hpfOct;

    const double tickRate = s.sampleRate / kControlInterval;
    s.fenv = dsp::AnalogEnv::makeCoefs({p.get(FenvAttack), p.get(FenvDecay), p.get(FenvSustain), p.get(FenvRelease)},
                                       tickRate);
    s.fenvVel = p.get(FenvVel);
    s.aenv = dsp::AnalogEnv::makeCoefs({p.get(AmpAttack), p.get(AmpDecay), p.get(AmpSustain), p.get(AmpRelease)},
                                       s.sampleRate);
    s.ampVel = p.get(AmpVel);

    const double tempo = ctx.transport.tempo > 1.0 ? ctx.transport.tempo : 120.0;
    for (int j = 0; j < 2; ++j) {
        const int base = j == 0 ? Lfo1Wave : Lfo2Wave;
        s.lfoWave[j] = clampv(toInt(p.get(base + 0)), 0, 5);
        const bool sync = p.get(base + 2) >= 0.5f;
        const int div = clampv(toInt(p.get(base + 3)), 0, dsp::kLfoDivisionCount - 1);
        const double beats = dsp::kLfoDivisionBeats[div];
        const double hz = sync ? tempo / 60.0 / beats : static_cast<double>(p.get(base + 1));
        s.lfoDelay[j] = p.get(base + 4);
        s.lfoKeySync[j] = p.get(base + 5) >= 0.5f;
        s.lfoIncPerTick[j] = hz * kControlInterval / s.sampleRate;
        s.lfoGlobalInc[j] = hz / s.sampleRate;
        if (sync && ctx.transport.playing) {
            lfoGlobal_[j] = ctx.transport.ppqPosition / beats; // lock to the transport
        }
        s.lfoGlobalPhase[j] = lfoGlobal_[j];
    }
    s.lfo1Pitch = p.get(Lfo1Pitch);
    s.lfo1Cutoff = p.get(Lfo1Cutoff);
    s.lfo1Amp = p.get(Lfo1Amp);

    for (int m = 0; m < kModSlots; ++m) {
        s.modSrc[m] = clampv(toInt(p.get(Mod1Src + 3 * m)), 0, SrcCount - 1);
        s.modDst[m] = clampv(toInt(p.get(Mod1Dst + 3 * m)), 0, DstCount - 1);
        s.modAmt[m] = p.get(Mod1Amt + 3 * m);
    }

    s.unison = clampv(toInt(p.get(Unison)), 1, kMaxUnison);
    s.unisonDetune = p.get(UnisonDetune);
    s.unisonSpread = p.get(UnisonSpread);
    s.glideSeconds = p.get(Glide);
    s.glideLegatoOnly = toInt(p.get(GlideMode)) == 1;
    s.drift = p.get(Drift);
    const auto mode = static_cast<VoiceMode>(clampv(toInt(p.get(VoiceModeP)), 0, 2));
    // Pan spread spreads poly voices; a mono/legato part (always voice 0) stays centred.
    s.panSpread = mode == VoiceMode::Poly ? p.get(PanSpread) : 0.0f;
    const ChannelState cs = ctx.channel ? *ctx.channel : ChannelState{};
    s.bendNorm = cs.pitchBend;
    s.bendSemis = cs.pitchBend * p.get(BendRange);
    s.modWheel = cs.modWheel;
    s.aftertouch = cs.aftertouch;
    s.expression = cs.expression;
    s.mwVibrato = p.get(MwVibrato);
    s.mwCutoff = p.get(MwCutoff);
    s.atVibrato = p.get(AtVibrato);
    s.atCutoff = p.get(AtCutoff);

    if (mode != mode_) {
        mode_ = mode;
        alloc_.setMode(mode);
    }
    alloc_.setPolyphony(std::min(toInt(p.get(Polyphony)), kSubVoiceBudget / s.unison));
    volume_.setTarget(dsp::dbToGain(p.get(VolumeDb)));
    drive_.setTarget(p.get(Drive));
}

// ------------------------------------------------------------------------------------------------ Voice

void VaSynth::Voice::noteOn(const VoiceStart& st) noexcept {
    const Shared& sh = *shared;
    const bool wasActive = aenv.isActive();
    targetNote = static_cast<float>(st.note);
    const bool glide = sh.glideSeconds > 0.0005f && st.glideFrom >= 0 &&
                       (!sh.glideLegatoOnly || st.legato || sh.fingeredLegato);
    if (glide) {
        // A stolen poly voice glides from the previously played note, not from its own old note.
        if (!wasActive || st.stolen) note = static_cast<float>(st.glideFrom);
        const float ticks = std::fmax(1.0f, sh.glideSeconds / sh.tickSeconds);
        glideStep = std::fabs(targetNote - note) / ticks; // constant time
    } else {
        note = targetNote;
        glideStep = 0.0f;
    }
    if (!st.legato) {
        velocity = st.velocity;
        seed = dsp::hash32(seed + 0x6d2b79f5u);
        random = dsp::bipolarFromBits(seed);
        timeSinceOn = 0.0f;
        const bool fresh = !wasActive || aenv.level() < 0.01f;
        const int prevSubs = wasActive ? numSubs : 0;
        // Sub-voice budget: polyphony is capped to budget / unison, but notes held across a unison change
        // (or re-struck voices above the cap) could still exceed it; never take more than what is left.
        int others = 0;
        for (int j = 0; j < kMaxVoices; ++j)
            if (j != index) others += sh.voiceSubs[j];
        numSubs = clampv(kSubVoiceBudget - others, 1, sh.unison);
        for (int i = 0; i < numSubs; ++i) {
            SubVoice& sv = subs[static_cast<size_t>(i)];
            if (fresh || i >= prevSubs) {
                sv.ladder.reset();
                sv.svf.reset();
                sv.hpf.reset();
                sv.driftTicks = 0;
                sv.gCur = -1.0f; // snap filter coefficient on the first control tick
            }
            if (fresh && sh.phaseReset) {
                for (int k = 0; k < kNumOscs; ++k) {
                    sv.osc[k].reset(0.0f);
                    sv.saw[k].reset(sv.seed + seed + static_cast<unsigned>(k));
                }
                sv.sub.reset(0.0f);
            }
        }
        for (int j = 0; j < 2; ++j)
            if (sh.lfoKeySync[j]) lfoPhase[j] = 0.0;
        if (!wasActive) gainCur = 0.0f;
        if (fresh) {
            smSnap = true;
            for (float& c : pwCur) c = -1.0f;
            curFilter = sh.filter;
        }
        aenv.noteOn();
        fenv.noteOn();
    }
    tickRemain = 0; // recompute control values before the next sample
}

void VaSynth::Voice::reset() noexcept {
    aenv.reset();
    fenv.reset();
    for (auto& sv : subs) {
        sv.ladder.reset();
        sv.svf.reset();
        sv.hpf.reset();
    }
    gainCur = gainInc = 0.0f;
    for (auto& sv : subs) sv.gCur = -1.0f;
    for (float& c : pwCur) c = -1.0f;
    smSnap = true;
    tickRemain = 0;
}

void VaSynth::Voice::control(int blockPos) noexcept {
    const Shared& sh = *shared;
    typeEpoch = sh.typeEpoch;
    if (smSnap) {
        for (int k = 0; k < SmCount; ++k) sm[k] = sh.smTarget[k];
        smSnap = false;
    } else {
        for (int k = 0; k < SmCount; ++k) sm[k] += (sh.smTarget[k] - sm[k]) * sh.smCoef;
    }
    if (sh.filter != curFilter) { // model switch mid-note: start the new filter from rest
        curFilter = sh.filter;
        for (int i = 0; i < numSubs; ++i) {
            subs[static_cast<size_t>(i)].ladder.reset();
            subs[static_cast<size_t>(i)].svf.reset();
            subs[static_cast<size_t>(i)].gCur = -1.0f;
        }
    }
    fenvValue = fenv.next(sh.fenv);
    timeSinceOn += sh.tickSeconds;
    if (note != targetNote) {
        if (glideStep <= 0.0f) note = targetNote;
        else if (note < targetNote) note = std::fmin(note + glideStep, targetNote);
        else note = std::fmax(note - glideStep, targetNote);
    }

    float lfo[2];
    for (int j = 0; j < 2; ++j) {
        const auto w = static_cast<dsp::LfoWave>(sh.lfoWave[j]);
        if (sh.lfoKeySync[j]) {
            lfo[j] = dsp::lfoShape(w, lfoPhase[j], seed + static_cast<uint32_t>(j));
            lfoPhase[j] += sh.lfoIncPerTick[j];
        } else {
            const double ph = sh.lfoGlobalPhase[j] + sh.lfoGlobalInc[j] * static_cast<double>(blockPos);
            lfo[j] = dsp::lfoShape(w, ph, 0x51u + static_cast<uint32_t>(j));
        }
        lfo[j] *= dsp::lfoDelayGain(timeSinceOn, sh.lfoDelay[j]);
    }

    // Mod matrix
    const float src[SrcCount] = {0.0f,          lfo[0],         lfo[1],         fenvValue,
                                 aenv.level(),  velocity,       (note - 60.0f) / 60.0f, sh.modWheel,
                                 sh.aftertouch, sh.bendNorm,    sh.expression,  random};
    float d[DstCount] = {};
    for (int m = 0; m < kModSlots; ++m) {
        const int s = sh.modSrc[m], t = sh.modDst[m];
        if (s != SrcOff && t != DstOff) d[t] += src[s] * sh.modAmt[m] * modDestScale(t);
    }

    const float l1 = lfo[0] * std::fmax(0.0f, 1.0f + d[DstLfo1Depth]);
    const float pitch = note + sh.bendSemis + l1 * sh.lfo1Pitch +
                        lfo[1] * (sh.modWheel * sh.mwVibrato + sh.aftertouch * sh.atVibrato) + d[DstPitch];
    float oscPitch[kNumOscs];
    float pw[kNumOscs];
    for (int k = 0; k < kNumOscs; ++k) {
        oscPitch[k] = pitch + sh.osc[k].pitchOffset + d[DstOsc1Pitch + k];
        pw[k] = clampv(sm[SmPw1 + k] + l1 * sh.osc[k].pwm * 0.45f + d[DstOsc1Pw + k], 0.02f, 0.98f);
        if (pwCur[k] < 0.0f) pwCur[k] = pw[k];
        pwInc[k] = (pw[k] - pwCur[k]) / static_cast<float>(kControlInterval);
        level[k] = clampv(sm[SmLevel1 + k] + d[DstOsc1Level + k], 0.0f, 1.5f);
    }
    oscPitch[0] += fenvValue * sh.pmEnv;
    subLevel = clampv(sm[SmSub] + d[DstSubLevel], 0.0f, 1.5f);
    noiseLevel = clampv(sm[SmNoise] + d[DstNoiseLevel], 0.0f, 1.5f);
    ringLevel = sm[SmRing];
    fm = clampv(sm[SmFm] + d[DstFm], 0.0f, 1.0f);
    driveGain = dsp::dbToGain(24.0f * sm[SmDrive]);
    driveComp = 1.0f / std::sqrt(driveGain);
    const float ssDetune = d[DstDetune];
    float ssCurve[kNumOscs];
    for (int k = 0; k < kNumOscs; ++k)
        ssCurve[k] = sh.osc[k].type != OscType::SuperSaw ? 0.0f
                     : ssDetune != 0.0f ? dsp::SuperSaw::detuneCurve(clampv(sh.osc[k].detune + ssDetune, 0.0f, 1.0f))
                                        : sh.ssCurve[k];

    const float envVel = (1.0f - sh.fenvVel) + sh.fenvVel * velocity;
    const float cutOct = sm[SmCutoff] + sh.keyTrack * (note - 60.0f) / 12.0f +
                         fenvValue * (sh.fenvAmt + d[DstFenvAmt]) * envVel + sh.filterVel * (velocity - 1.0f) +
                         l1 * sh.lfo1Cutoff + lfo[1] * sh.modWheel * sh.mwCutoff + sh.aftertouch * sh.atCutoff +
                         d[DstCutoff];
    const float res = clampv(sm[SmResonance] + d[DstResonance], 0.0f, 1.0f);
    const bool ladder = sh.filter != FilterType::Sem;
    fbK = ladder ? dsp::LadderFilter::feedbackFor(res, sh.filter == FilterType::Moog ? dsp::LadderFilter::Model::Transistor
                                                                                   : dsp::LadderFilter::Model::Ota)
               : 0.0f;
    svfK = dsp::Svf::dampingFor(res * 0.97f);
    const float hpfOct = clampv(sm[SmHpf] + d[DstHpf], 3.0f, 14.5f);
    hpfOn = hpfOct > 3.6f; // > ~12 Hz
    hpfG = dsp::tptG(std::exp2(hpfOct), sh.sr);

    const float velGain = (1.0f - sh.ampVel) + sh.ampVel * velocity;
    // l1 can exceed +-1 via the LFO1 Depth mod destination: keep the tremolo gain non-negative.
    const float trem = std::fmax(0.0f, 1.0f - sh.lfo1Amp * (0.5f + 0.5f * l1));
    const float gain = kVoiceGain * velGain * clampv(1.0f + d[DstAmp], 0.0f, 2.0f) * trem /
           std::sqrt(static_cast<float>(numSubs));
    gainInc = (gain - gainCur) / static_cast<float>(kControlInterval);
    silentHeld = aenv.stage() == dsp::AnalogEnv::Stage::Sustain && aenv.level() < 1e-6f;

    const float basePan = voicePanPattern(index) * sh.panSpread + d[DstPan];
    const float driftCoef = sh.driftCoef;
    const float maxHz = sh.sr * 0.45f;
    for (int i = 0; i < numSubs; ++i) {
        SubVoice& sv = subs[static_cast<size_t>(i)];
        const float pos = numSubs > 1 ? -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(numSubs - 1) : 0.0f;
        if (--sv.driftTicks <= 0) {
            sv.seed = dsp::hash32(sv.seed + 1u);
            sv.driftTarget = dsp::bipolarFromBits(sv.seed);
            sv.driftCutTarget = dsp::bipolarFromBits(dsp::hash32(sv.seed ^ 0xabcdefu));
            sv.driftTicks = static_cast<int>(static_cast<float>(100u + sv.seed % 400u) * sh.driftTickScale);
        }
        sv.driftValue += (sv.driftTarget - sv.driftValue) * driftCoef;
        sv.driftCut += (sv.driftCutTarget - sv.driftCut) * driftCoef;
        const float detune = pos * sh.unisonDetune * 0.005f + sh.drift * (sv.driftValue * 0.08f + sv.driftStatic * 0.05f);
        for (int o = 0; o < kNumOscs; ++o) {
            sv.inc[o] = dsp::noteToHz(oscPitch[o] + detune) * sh.invSr;
            if (sh.osc[o].type == OscType::SuperSaw) {
                sv.saw[o].setShapeCurve(ssCurve[o], (sm[SmPw1 + o] - 0.05f) / 0.9f);
                sv.saw[o].setInc(sv.inc[o]);
            }
        }
        const float fc = clampv(std::exp2(cutOct + sh.drift * (sv.driftCut * 0.15f + sv.driftCutStatic * 0.1f)),
                                8.0f, maxHz);
        // Ladder: G = g/(1+g); SVF: g = tan(pi fc/fs). Both ramped linearly per sample over the tick.
        const float g = ladder ? dsp::LadderFilter::coefG(fc, sh.sr) : std::tan(dsp::kPi * fc * sh.invSr);
        if (sv.gCur < 0.0f) sv.gCur = g;
        sv.gInc = (g - sv.gCur) / static_cast<float>(kControlInterval);
        sv.hpf.setG(hpfG);
        dsp::panGains(clampv(basePan + pos * sh.unisonSpread, -1.0f, 1.0f), sv.panL, sv.panR);
    }
}

namespace {

// fmScale: per-sample FM factor already applied to `inc` (used by the supersaw, whose increments are per saw).
inline float tickOsc(VaSynth::SubVoice& sv, int k, VaSynth::OscType t, float inc, float pw, float syncD,
                     float fmScale = 1.0f) noexcept {
    switch (t) {
    case VaSynth::OscType::Saw: return sv.osc[k].tick(dsp::OscWave::Saw, inc, 0.5f, syncD);
    case VaSynth::OscType::Pulse: return sv.osc[k].tick(dsp::OscWave::Pulse, inc, pw, syncD);
    case VaSynth::OscType::Triangle: return sv.osc[k].tick(dsp::OscWave::Triangle, inc, 0.5f, syncD);
    case VaSynth::OscType::Sine: return sv.osc[k].tick(dsp::OscWave::Sine, inc, 0.5f, syncD);
    case VaSynth::OscType::SuperSaw: return sv.saw[k].tick(fmScale);
    case VaSynth::OscType::Noise: return sv.noise.next();
    }
    return 0.0f;
}

inline float masterWrap(const VaSynth::SubVoice& sv, int k, VaSynth::OscType t) noexcept {
    if (t == VaSynth::OscType::SuperSaw) return sv.saw[k].wrapD();
    if (t == VaSynth::OscType::Noise) return -1.0f;
    return sv.osc[k].wrapD();
}

} // namespace

void VaSynth::Voice::renderChunk(float* left, float* right, int n) noexcept {
    const Shared& sh = *shared;
    float env[kControlInterval];
    for (int i = 0; i < n; ++i) env[i] = aenv.next(sh.aenv);
    if (silentHeld) return;

    // Gain and ladder coefficient ramp linearly over the control period (state carried across chunks).
    const float gainStep = gainInc;
    const float gain0 = gainCur;

    const OscType t0 = sh.osc[0].type, t1 = sh.osc[1].type, t2 = sh.osc[2].type;
    const bool need3 = level[2] > 0.0f || fm > 0.0f;
    const bool need2 = level[1] > 0.0f || ringLevel > 0.0f || (sh.sync && t0 != OscType::SuperSaw);
    const bool sync = sh.sync && t0 != OscType::SuperSaw && t0 != OscType::Noise;
    const bool needSub = subLevel > 0.0f;
    const bool needNoise = noiseLevel > 0.0f;
    const bool useHpf = hpfOn;
    const float fmDepth = fm * 4.0f;
    const float subDivInv = 1.0f / static_cast<float>(sh.subDiv);
    const float l0 = level[0] * 0.5f, l1 = level[1] * 0.5f, l2 = level[2] * 0.5f;
    const float ls = subLevel * 0.5f, ln = noiseLevel * 0.5f, lr = ringLevel * 0.7f;
    const FilterType ft = sh.filter;
    const auto model = ft == FilterType::Moog ? dsp::LadderFilter::Model::Transistor : dsp::LadderFilter::Model::Ota;
    const auto resp = static_cast<dsp::LadderFilter::Response>(sh.filterMode);
    const bool svfDrive = driveGain > 1.01f; // driveGain/driveComp/svfK: control-rate members

    for (int s = 0; s < numSubs; ++s) {
        SubVoice& sv = subs[static_cast<size_t>(s)];
        const float gStep = sv.gInc;
        float g = sv.gCur;
        float gn = gain0;
        const float inc0 = sv.inc[0], inc1 = sv.inc[1], inc2 = sv.inc[2];
        float pw0 = pwCur[0], pw1 = pwCur[1], pw2 = pwCur[2];
        const float pwS0 = pwInc[0], pwS1 = pwInc[1], pwS2 = pwInc[2];
        for (int i = 0; i < n; ++i) {
            pw0 += pwS0;
            pw1 += pwS1;
            pw2 += pwS2;
            const float o3 = need3 ? tickOsc(sv, 2, t2, inc2, pw2, -1.0f) : 0.0f;
            const float o2 = need2 ? tickOsc(sv, 1, t1, inc1, pw1, -1.0f) : 0.0f;
            const float syncD = sync ? masterWrap(sv, 1, t1) : -1.0f;
            const float fmScale = fmDepth > 0.0f ? std::fmax(0.0f, 1.0f + fmDepth * o3) : 1.0f;
            const float o1 = tickOsc(sv, 0, t0, inc0 * fmScale, pw0, syncD, fmScale);
            float x = l0 * o1 + l1 * o2 + l2 * o3;
            if (needSub) x += ls * sv.sub.tick(dsp::OscWave::Pulse, inc0 * subDivInv, 0.5f);
            if (needNoise) x += ln * sv.noise.next();
            if (lr > 0.0f) x += lr * o1 * o2;
            if (useHpf) x = sv.hpf.hp(x);
            float y;
            if (ft == FilterType::Sem) {
                const float xin = svfDrive ? dsp::fastTanh(x * driveGain) : x;
                sv.svf.setGK(g, svfK);
                g += gStep;
                const dsp::Svf::Out f = sv.svf.tick(xin);
                switch (sh.filterMode) {
                case 1: y = f.bp; break;
                case 2: y = f.hp; break;
                case 3: y = f.lp + f.hp; break;
                default: y = f.lp; break;
                }
                if (svfDrive) y *= driveComp * 1.5f;
            } else {
                y = sv.ladder.process(x * driveGain, g, fbK, model, resp) * driveComp;
                g += gStep;
            }
            const float a = y * env[i] * gn;
            gn += gainStep;
            left[i] += a * sv.panL;
            right[i] += a * sv.panR;
        }
    }
    gainCur = gain0 + gainStep * static_cast<float>(n);
    for (int k = 0; k < kNumOscs; ++k) pwCur[k] += pwInc[k] * static_cast<float>(n);
    for (int s = 0; s < numSubs; ++s) {
        SubVoice& sv = subs[static_cast<size_t>(s)];
        sv.gCur += sv.gInc * static_cast<float>(n);
    }
}

void VaSynth::Voice::render(float* left, float* right, int n) noexcept {
    int i = 0;
    if (typeEpoch != shared->typeEpoch) tickRemain = 0; // osc type changed: refresh oscillator setup now
    while (i < n) {
        if (tickRemain <= 0) {
            control(shared->blockPos + i);
            tickRemain = kControlInterval;
        }
        const int c = std::min(tickRemain, n - i);
        renderChunk(left + i, right + i, c);
        tickRemain -= c;
        i += c;
        if (!aenv.isActive()) break;
    }
}

// ------------------------------------------------------------------------------------------------ Module

void VaSynth::renderSegment(AudioBlock& out, int start, int end) noexcept {
    if (end <= start) return;
    shared_.blockPos = start;
    alloc_.forEachActive([&](Voice& v) { v.render(out.left + start, out.right + start, end - start); });
}

void VaSynth::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    updateShared(ctx);
    alloc_.setSustainEnabled(true);
    int pos = 0;
    for (const MidiEvent& e : events) {
        const int at = std::clamp(static_cast<int>(e.sampleOffset), pos, out.numSamples);
        renderSegment(out, pos, at);
        pos = at;
        switch (e.type) {
        case MidiEventType::NoteOn:
            shared_.fingeredLegato = keysDown_ > 0;
            if (!keyDown_[e.data1 & 127]) {
                keyDown_[e.data1 & 127] = true;
                ++keysDown_;
            }
            break;
        case MidiEventType::NoteOff:
            if (keyDown_[e.data1 & 127]) {
                keyDown_[e.data1 & 127] = false;
                --keysDown_;
            }
            shared_.fingeredLegato = keysDown_ > 0; // returning to a held note in mono mode
            break;
        case MidiEventType::AllNotesOff:
        case MidiEventType::AllSoundOff:
            keyDown_.fill(false);
            keysDown_ = 0;
            break;
        default: break;
        }
        for (int v = 0; v < kMaxVoices; ++v) {
            const Voice& vv = alloc_.voice(v);
            shared_.voiceSubs[v] = vv.isActive() ? vv.numSubs : 0;
        }
        alloc_.handleEvent(e);
    }
    renderSegment(out, pos, out.numSamples);

    // Output stage: soft saturation, volume, DC blocker.
    for (int i = 0; i < out.numSamples; ++i) {
        const float vol = volume_.next();
        const float dr = drive_.next();
        float l = out.left[i], r = out.right[i];
        if (dr > 0.0f) {
            const float g = 1.0f + 5.0f * dr;
            const float c = dr / std::sqrt(g);
            l = l * (1.0f - dr) + c * dsp::fastTanh(g * l);
            r = r * (1.0f - dr) + c * dsp::fastTanh(g * r);
        }
        l *= vol;
        r *= vol;
        const float hl = l - dcL_;
        const float hr = r - dcR_;
        dcL_ += (1.0f - dcCoef_) * hl;
        dcR_ += (1.0f - dcCoef_) * hr;
        out.left[i] = hl;
        out.right[i] = hr;
    }
    for (int j = 0; j < 2; ++j) {
        lfoGlobal_[j] += shared_.lfoGlobalInc[j] * out.numSamples;
        if (lfoGlobal_[j] > 1.0e6) lfoGlobal_[j] -= std::floor(lfoGlobal_[j]);
    }
    activeSubVoices_.store(activeSubVoices(), std::memory_order_relaxed);
}

} // namespace ks
