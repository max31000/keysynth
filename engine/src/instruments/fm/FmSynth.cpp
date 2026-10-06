#include "instruments/fm/FmSynth.h"

#include "core/AppPaths.h"
#include "core/VoiceAllocator.h"
#include "dsp/Math.h"
#include "instruments/fm/Dx7Voice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

// Vendored MSFA (SYSTEM include dirs: its warnings don't apply). Included last: it defines global N/min/max.
#include "controllers.h"
#include "dexed/EngineMkI.h"
#include "dexed/EngineOpl.h"
#include "dx7note.h"
#include "env.h"
#include "exp2.h"
#include "fm_core.h"
#include "freqlut.h"
#include "lfo.h"
#include "pitchenv.h"
#include "porta.h"
#include "sin.h"

namespace ks {

namespace vced = fm::vced;

namespace {

constexpr int kChunk = N; // MSFA block size (8, third_party/msfa/synth.h): events take effect <= 7 samples late
static_assert(kChunk == 8);
constexpr float kBaseGain = 2.0f; // one full-level carrier ~ -12 dBFS
constexpr int kQuietSamples = 1024;     // released voice silent this long (~21 ms @ 48k) -> free it
constexpr int kQuietChunks = kQuietSamples / kChunk;
constexpr float kGainRampSamples = 64.0f; // volume changes ramp linearly over this many samples (no zipper)
constexpr float kQuietLevel = 1.0e-4f;  // ~3 LSB of msfa's 16-bit voice output (-80 dBFS per voice)

const std::vector<std::string> kCurves = {"-LIN", "-EXP", "+EXP", "+LIN"};

// MSFA lookup tables are process globals. SR-independent ones once; SR-dependent ones when the rate changes
// (prepare runs on the control thread; a different rate only occurs after the device was stopped).
void ensureTables(double sampleRate) {
    static std::mutex m;
    static bool staticDone = false;
    static double tableRate = 0.0;
    const std::lock_guard<std::mutex> lock(m);
    if (!staticDone) {
        Exp2::init();
        Tanh::init();
        Sin::init();
        staticDone = true;
    }
    if (sampleRate > 0.0 && sampleRate != tableRate) {
        Freqlut::init(sampleRate);
        Lfo::init(sampleRate);
        PitchEnv::init(sampleRate);
        Env::init_sr(sampleRate);
        Porta::init_sr(sampleRate);
        tableRate = sampleRate;
    }
}

std::shared_ptr<TuningState> standardTuning() {
    static const std::shared_ptr<TuningState> t = createStandardTuning();
    return t;
}

inline int iparam(const ParamSet& p, int idx) noexcept { return static_cast<int>(std::lround(p.get(idx))); }
inline uint8_t clamp99(int v) noexcept { return static_cast<uint8_t>(std::clamp(v, 0, 99)); }

} // namespace

// ----------------------------------------------------------------------------------------------------------
struct FmSynth::Impl {
    struct Voice {
        Impl* im = nullptr;
        Dx7Note note;
        bool playing = false;
        bool released = false;
        int quiet = 0;
        int midiNote = 60; // after transpose
        int velocity = 100;

        Voice() : note(standardTuning(), nullptr) {}

        void noteOn(const VoiceStart& s) noexcept {
            const int transpose = static_cast<int>(im->patch[vced::Transpose]) - 24;
            const int pitch = std::clamp(s.note + transpose, 0, 127);
            const int vel = std::clamp(static_cast<int>(std::lround(s.velocity * 127.0f)), 1, 127);
            if (s.legato && playing) {
                note.update(im->patch.data(), pitch, vel, 1);
            } else {
                const bool wasPlaying = playing;
                note.init(im->patch.data(), pitch, vel, 1, &im->ctrl);
                // DX7 osc key sync; as in Dexed, not on a stolen voice (avoids a click).
                if (im->patch[vced::OscSync] != 0 && !wasPlaying) note.oscSync();
            }
            midiNote = pitch;
            velocity = vel;
            playing = true;
            released = false;
            quiet = 0;
        }
        void noteOff() noexcept {
            if (!playing) return;
            note.keyup();
            released = true;
        }
        void kill() noexcept {} // the following noteOn re-inits the note (phases kept)
        void reset() noexcept {
            note.forget();
            playing = false;
            released = false;
            quiet = 0;
        }
        bool isActive() const noexcept { return playing; }

        // Adds one kChunk-sample chunk into `acc`.
        void render(float* acc, int32_t lfoVal, int32_t lfoDelay) noexcept {
            alignas(16) int32_t buf[kChunk];
            std::memset(buf, 0, sizeof(buf));
            note.compute(buf, lfoVal, lfoDelay, &im->ctrl);
            float peak = 0.0f;
            for (int j = 0; j < kChunk; ++j) {
                // Same fixed-point -> float conversion as Dexed (per-voice clip at +-1).
                const int32_t val = buf[j] >> 4;
                const int clip = val < -(1 << 24) ? -0x8000 : (val >= (1 << 24) ? 0x7fff : val >> 9);
                const float f = static_cast<float>(clip) * (1.0f / 32768.0f);
                acc[j] += f;
                peak = std::max(peak, std::fabs(f));
            }
            if (!note.isPlaying()) {
                playing = false;
            } else if (released) {
                quiet = peak < kQuietLevel ? quiet + 1 : 0;
                if (quiet >= kQuietChunks) playing = false;
            }
        }
    };

    Controllers ctrl;
    FmCore msfaCore;
    EngineMkI mkICore;
    EngineOpl oplCore;
    Lfo lfo{}; // no constructor upstream: value-initialize (phase/random state)
    std::array<uint8_t, 156> patch{};
    VoiceAllocator<Voice, kMaxVoices> alloc;
    VoiceMode mode = VoiceMode::Poly;
    alignas(16) float chunk[kChunk]{};
    int chunkPos = kChunk; // consumed samples of `chunk` (kChunk = empty)
    uint32_t paramVersion = 0;
    bool built = false;
    float gain = 0.0f;
    float gainTarget = 0.0f;
    float gainStep = 0.0f; // per sample, set when gainTarget changes
    // DC blocker (the DX7 output stage is AC-coupled; 1:1 FM ratios and feedback produce DC).
    float dcCoef = 0.999f;
    float dcX1 = 0.0f, dcY1 = 0.0f;
    double sampleRate = 48000.0;

    Impl() {
        for (int i = 0; i < kMaxVoices; ++i) alloc.voice(i).im = this;
        std::memset(ctrl.values_, 0, sizeof(ctrl.values_));
        ctrl.values_[kControllerPitch] = 0x2000;
        ctrl.values_[kControllerPitchRangeUp] = 2;
        ctrl.values_[kControllerPitchRangeDn] = 2;
        ctrl.values_[kControllerPitchStep] = 0;
        ctrl.masterTune = 0;
        ctrl.modwheel_cc = ctrl.breath_cc = ctrl.foot_cc = ctrl.aftertouch_cc = 0;
        ctrl.portamento_enable_cc = false;
        ctrl.portamento_cc = 0;
        ctrl.portamento_gliss_cc = false;
        ctrl.mpeEnabled = false;
        ctrl.core = &msfaCore;
        ctrl.refresh();
        const fm::Dx7Voice init = fm::Dx7Voice::initVoice();
        patch = init.data;
        lfo.reset(patch.data() + vced::LfoSpeed);
    }

    // Builds the DX7 voice from params + macros into `out` (RT-safe: reads atomics only).
    static void buildPatch(const ParamSet& p, std::array<uint8_t, 156>& out) noexcept {
        const fm::Dx7Voice raw = FmSynth::paramsToVoice(p);
        out = raw.data;
        const int alg = out[vced::Algorithm];
        const int attack = static_cast<int>(std::lround(p.get(Attack) * 45.0f));
        const int release = static_cast<int>(std::lround(p.get(Release) * 45.0f));
        const int bright = static_cast<int>(std::lround(p.get(Brightness) * 24.0f));
        for (int n = 1; n <= 6; ++n) {
            uint8_t* o = out.data() + vced::opBase(n);
            o[vced::R1] = clamp99(o[vced::R1] - attack);
            o[vced::R4] = clamp99(o[vced::R4] - release);
            // msfa data index: OP6 = 0 ... OP1 = 5.
            if (bright != 0 && !FmCore::isCarrier(alg, 6 - n)) o[vced::OutLevel] = clamp99(o[vced::OutLevel] + bright);
        }
    }

    void updateControllers(const ParamSet& p, const ProcessContext& ctx) noexcept {
        const ChannelState cs = ctx.channel ? *ctx.channel : ChannelState{};
        const int pb = std::clamp(0x2000 + static_cast<int>(std::lround(cs.pitchBend * 8191.0f)), 0, 0x3fff);
        ctrl.values_[kControllerPitch] = pb;
        const int range = iparam(p, PbRange);
        ctrl.values_[kControllerPitchRangeUp] = range;
        ctrl.values_[kControllerPitchRangeDn] = range;
        ctrl.masterTune = static_cast<int>(std::lround(p.get(Tune) * static_cast<float>(1 << 24) / 1200.0f));
        ctrl.modwheel_cc = std::clamp(static_cast<int>(std::lround(cs.modWheel * 127.0f)), 0, 127);
        ctrl.aftertouch_cc = std::clamp(static_cast<int>(std::lround(cs.aftertouch * 127.0f)), 0, 127);
        ctrl.wheel.range = iparam(p, MwRange);
        ctrl.wheel.pitch = p.get(MwPitch) >= 0.5f;
        ctrl.wheel.amp = p.get(MwAmp) >= 0.5f;
        ctrl.wheel.eg = p.get(MwEg) >= 0.5f;
        ctrl.at.range = iparam(p, AtRange);
        ctrl.at.pitch = p.get(AtPitch) >= 0.5f;
        ctrl.at.amp = p.get(AtAmp) >= 0.5f;
        ctrl.at.eg = p.get(AtEg) >= 0.5f;
        ctrl.refresh();
        for (int n = 1; n <= 6; ++n) ctrl.opSwitch[6 - n] = p.get(opParam(n, OpOn)) >= 0.5f ? '1' : '0';
        ctrl.opSwitch[6] = 0;
        switch (iparam(p, EngineModel)) {
        case 1: ctrl.core = &mkICore; break;
        case 2: ctrl.core = &oplCore; break;
        default: ctrl.core = &msfaCore; break;
        }
    }

    // Rebuild the voice when params changed; re-apply it to sounding notes only if DX7 data changed.
    void updatePatch(const ParamSet& p) noexcept {
        const uint32_t v = p.version();
        if (built && v == paramVersion) return;
        paramVersion = v;
        std::array<uint8_t, 156> next{};
        buildPatch(p, next);
        const bool changed = !built || std::memcmp(next.data(), patch.data(), vced::Name) != 0;
        built = true;
        if (!changed) return;
        patch = next;
        lfo.reset(patch.data() + vced::LfoSpeed);
        for (int i = 0; i < kMaxVoices; ++i) {
            Voice& vc = alloc.voice(i);
            if (vc.playing) vc.note.update(patch.data(), vc.midiNote, vc.velocity, 1);
        }
    }

    void handleEvent(const MidiEvent& e) noexcept {
        if (e.type == MidiEventType::NoteOn) {
            bool anyHeld = false;
            for (int i = 0; i < kMaxVoices && !anyHeld; ++i) anyHeld = alloc.isHeld(i) && alloc.voice(i).playing;
            if (!anyHeld) lfo.keydown(); // LFO key sync restarts on the first key (DX7)
        }
        alloc.handleEvent(e);
    }

    void renderChunk() noexcept {
        std::memset(chunk, 0, sizeof(chunk));
        const int32_t lfoVal = lfo.getsample();
        const int32_t lfoDelay = lfo.getdelay();
        alloc.forEachActive([&](Voice& v) { v.render(chunk, lfoVal, lfoDelay); });
        // Linear gain ramp towards gainTarget over kGainRampSamples (volume changes don't zipper).
        for (int j = 0; j < kChunk; ++j) {
            if (gain != gainTarget) {
                gain += gainStep;
                if ((gainStep > 0.0f) == (gain >= gainTarget)) gain = gainTarget;
            }
            const float x = chunk[j] * gain;
            const float y = x - dcX1 + dcCoef * dcY1;
            dcX1 = x;
            dcY1 = std::fabs(y) < 1.0e-20f ? 0.0f : y; // flush denormals
            chunk[j] = y;
        }
        chunkPos = 0;
    }
};

// ----------------------------------------------------------------------------------------------------------
const ModuleInfo& FmSynth::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "fm";
        i.displayName = "FM (DX7)";
        i.kind = ModuleKind::Instrument;
        i.category = "Synth";
        auto& ps = i.params;
        ps.reserve(static_cast<size_t>(kParamCount));
        // Main / macros / controllers (non-DX7)
        ps.push_back(linearParam("volume_db", "Volume", -40.0f, 12.0f, 0.0f, "dB", "Main"));
        ps.push_back(intParam("voices", "Voices", 1, kMaxVoices, 16, {}, "Main"));
        ps.push_back(enumParam("engine_model", "Engine", {"Modern", "Mark I", "OPL"}, 0, "Main"));
        ps.push_back(enumParam("voice_mode", "Voice Mode", {"Poly", "Mono", "Legato"}, 0, "Main"));
        ps.push_back(linearParam("brightness", "Brightness", -1.0f, 1.0f, 0.0f, {}, "Macro"));
        ps.push_back(linearParam("attack", "Attack", -1.0f, 1.0f, 0.0f, {}, "Macro"));
        ps.push_back(linearParam("release", "Release", -1.0f, 1.0f, 0.0f, {}, "Macro"));
        ps.push_back(linearParam("tune", "Tune", -100.0f, 100.0f, 0.0f, "ct", "Macro"));
        ps.push_back(intParam("pb_range", "Bend Range", 0, 12, 2, "st", "Controllers"));
        ps.push_back(intParam("mw_range", "Wheel Range", 0, 99, 50, {}, "Controllers"));
        ps.push_back(boolParam("mw_pitch", "Wheel > Pitch", true, "Controllers"));
        ps.push_back(boolParam("mw_amp", "Wheel > Amp", false, "Controllers"));
        ps.push_back(boolParam("mw_eg", "Wheel > EG Bias", false, "Controllers"));
        ps.push_back(intParam("at_range", "AT Range", 0, 99, 0, {}, "Controllers"));
        ps.push_back(boolParam("at_pitch", "AT > Pitch", false, "Controllers"));
        ps.push_back(boolParam("at_amp", "AT > Amp", false, "Controllers"));
        ps.push_back(boolParam("at_eg", "AT > EG Bias", false, "Controllers"));
        // DX7 global voice params
        ps.push_back(intParam("alg", "Algorithm", 1, 32, 1, {}, "Main"));
        ps.push_back(intParam("feedback", "Feedback", 0, 7, 0, {}, "Main"));
        ps.push_back(boolParam("osc_sync", "Osc Key Sync", true, "Global"));
        ps.push_back(intParam("transpose", "Transpose", -24, 24, 0, "st", "Global"));
        ps.push_back(intParam("lfo_speed", "LFO Speed", 0, 99, 35, {}, "LFO"));
        ps.push_back(intParam("lfo_delay", "LFO Delay", 0, 99, 0, {}, "LFO"));
        ps.push_back(intParam("lfo_pmd", "LFO Pitch Depth", 0, 99, 0, {}, "LFO"));
        ps.push_back(intParam("lfo_amd", "LFO Amp Depth", 0, 99, 0, {}, "LFO"));
        ps.push_back(boolParam("lfo_sync", "LFO Key Sync", true, "LFO"));
        ps.push_back(enumParam("lfo_wave", "LFO Wave", {"Triangle", "Saw Down", "Saw Up", "Square", "Sine", "S&H"}, 0,
                               "LFO"));
        ps.push_back(intParam("pitch_mod_sens", "Pitch Mod Sens", 0, 7, 3, {}, "LFO"));
        for (int k = 1; k <= 4; ++k)
            ps.push_back(intParam("peg_rate" + std::to_string(k), "PEG Rate " + std::to_string(k), 0, 99, 99, {},
                                  "Pitch EG"));
        for (int k = 1; k <= 4; ++k)
            ps.push_back(intParam("peg_level" + std::to_string(k), "PEG Level " + std::to_string(k), 0, 99, 50, {},
                                  "Pitch EG"));
        // Operators
        for (int n = 1; n <= 6; ++n) {
            const std::string id = "op" + std::to_string(n) + "_";
            const std::string nm = "Op" + std::to_string(n) + " ";
            const std::string g = "Op " + std::to_string(n);
            for (int k = 1; k <= 4; ++k)
                ps.push_back(intParam(id + "eg_rate" + std::to_string(k), nm + "Rate " + std::to_string(k), 0, 99, 99,
                                      {}, g));
            for (int k = 1; k <= 4; ++k)
                ps.push_back(intParam(id + "eg_level" + std::to_string(k), nm + "Level " + std::to_string(k), 0, 99,
                                      k == 4 ? 0 : 99, {}, g));
            ps.push_back(intParam(id + "kls_break", nm + "Break Point", 0, 99, 39, {}, g));
            ps.push_back(intParam(id + "kls_left_depth", nm + "Left Depth", 0, 99, 0, {}, g));
            ps.push_back(intParam(id + "kls_right_depth", nm + "Right Depth", 0, 99, 0, {}, g));
            ps.push_back(enumParam(id + "kls_left_curve", nm + "Left Curve", kCurves, 0, g));
            ps.push_back(enumParam(id + "kls_right_curve", nm + "Right Curve", kCurves, 0, g));
            ps.push_back(intParam(id + "rate_scale", nm + "Rate Scaling", 0, 7, 0, {}, g));
            ps.push_back(intParam(id + "ams", nm + "Amp Mod Sens", 0, 3, 0, {}, g));
            ps.push_back(intParam(id + "vel_sens", nm + "Velocity Sens", 0, 7, 0, {}, g));
            ps.push_back(intParam(id + "level", nm + "Output Level", 0, 99, n == 1 ? 99 : 0, {}, g));
            ps.push_back(enumParam(id + "mode", nm + "Osc Mode", {"Ratio", "Fixed"}, 0, g));
            ps.push_back(intParam(id + "coarse", nm + "Coarse", 0, 31, 1, {}, g));
            ps.push_back(intParam(id + "fine", nm + "Fine", 0, 99, 0, {}, g));
            ps.push_back(intParam(id + "detune", nm + "Detune", -7, 7, 0, {}, g));
            ps.push_back(boolParam(id + "on", nm + "On", true, g));
        }
        i.uiHints = {
            {"groupOrder", {"Main", "Macro", "Op 1", "Op 2", "Op 3", "Op 4", "Op 5", "Op 6", "LFO", "Pitch EG",
                            "Global", "Controllers"}},
            {"front", {"alg", "feedback", "op1_level", "op2_level", "op3_level", "op4_level", "op5_level",
                       "op6_level", "brightness", "attack", "release", "volume_db"}},
            // Tabbed panel (UI ModulePanel): one tab per operator.
            {"tabs",
             {{{"name", "Voice"}, {"groups", {"Main", "Macro"}}},
              {{"name", "Op 1"}, {"groups", {"Op 1"}}},
              {{"name", "Op 2"}, {"groups", {"Op 2"}}},
              {{"name", "Op 3"}, {"groups", {"Op 3"}}},
              {{"name", "Op 4"}, {"groups", {"Op 4"}}},
              {{"name", "Op 5"}, {"groups", {"Op 5"}}},
              {{"name", "Op 6"}, {"groups", {"Op 6"}}},
              {{"name", "LFO / Pitch EG"}, {"groups", {"LFO", "Pitch EG"}}},
              {{"name", "Global / Ctrl"}, {"groups", {"Global", "Controllers"}}}}}};
        return i;
    }();
    return info;
}

FmSynth::FmSynth() : Module(moduleInfo()) {
    ensureTables(0.0);
    impl_ = std::make_unique<Impl>();
}

FmSynth::~FmSynth() = default;

void FmSynth::prepare(double sampleRate, int /*maxBlock*/) {
    ensureTables(sampleRate);
    impl_->sampleRate = sampleRate;
    impl_->dcCoef = static_cast<float>(std::exp(-2.0 * 3.14159265358979 * 8.0 / sampleRate)); // ~8 Hz high-pass
    impl_->built = false;
    impl_->updatePatch(params());
    impl_->gain = impl_->gainTarget = kBaseGain * dsp::dbToGain(params().get(VolumeDb));
    impl_->gainStep = 0.0f;
    reset();
}

void FmSynth::reset() {
    impl_->alloc.reset();
    impl_->chunkPos = kChunk;
    impl_->dcX1 = impl_->dcY1 = 0.0f;
    activeVoices_.store(0, std::memory_order_relaxed);
}

int FmSynth::tailSamples() const { return static_cast<int>(impl_->sampleRate * 2.0); }

void FmSynth::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    Impl& im = *impl_;
    const ParamSet& p = params();
    im.updatePatch(p);
    im.updateControllers(p, ctx);
    const auto mode = static_cast<VoiceMode>(std::clamp(iparam(p, VoiceModeP), 0, 2));
    if (mode != im.mode) {
        im.mode = mode;
        im.alloc.setMode(mode);
    }
    im.alloc.setPolyphony(iparam(p, Voices));
    const float target = kBaseGain * dsp::dbToGain(p.get(VolumeDb));
    if (target != im.gainTarget) {
        im.gainTarget = target;
        im.gainStep = (target - im.gain) / kGainRampSamples;
    }

    size_t ev = 0;
    int pos = 0;
    const int n = out.numSamples;
    while (pos < n) {
        if (im.chunkPos >= kChunk) {
            // Events up to this position take effect from this chunk on.
            while (ev < events.size() && static_cast<int>(events[ev].sampleOffset) <= pos) im.handleEvent(events[ev++]);
            im.renderChunk();
        }
        const int take = std::min(kChunk - im.chunkPos, n - pos);
        std::memcpy(out.left + pos, im.chunk + im.chunkPos, sizeof(float) * static_cast<size_t>(take));
        im.chunkPos += take;
        pos += take;
    }
    // Events inside the already-rendered remainder apply from the next chunk.
    while (ev < events.size()) im.handleEvent(events[ev++]);
    std::memcpy(out.right, out.left, sizeof(float) * static_cast<size_t>(n));
    activeVoices_.store(im.alloc.activeCount(), std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------------------------------------
fm::Dx7Voice FmSynth::paramsToVoice(const ParamSet& p) {
    fm::Dx7Voice v;
    auto& d = v.data;
    for (int n = 1; n <= 6; ++n) {
        uint8_t* o = d.data() + vced::opBase(n);
        for (int f = 0; f < OpDetune; ++f) o[f] = static_cast<uint8_t>(std::max(0, iparam(p, opParam(n, f))));
        o[vced::Detune] = static_cast<uint8_t>(std::clamp(iparam(p, opParam(n, OpDetune)) + 7, 0, 14));
    }
    for (int k = 0; k < 4; ++k) {
        d[static_cast<size_t>(vced::PR1 + k)] = static_cast<uint8_t>(iparam(p, PegRate1 + k));
        d[static_cast<size_t>(vced::PL1 + k)] = static_cast<uint8_t>(iparam(p, PegLevel1 + k));
    }
    d[vced::Algorithm] = static_cast<uint8_t>(std::clamp(iparam(p, Alg) - 1, 0, 31));
    d[vced::Feedback] = static_cast<uint8_t>(iparam(p, Feedback));
    d[vced::OscSync] = static_cast<uint8_t>(iparam(p, OscSync));
    d[vced::LfoSpeed] = static_cast<uint8_t>(iparam(p, LfoSpeed));
    d[vced::LfoDelay] = static_cast<uint8_t>(iparam(p, LfoDelay));
    d[vced::LfoPmd] = static_cast<uint8_t>(iparam(p, LfoPmd));
    d[vced::LfoAmd] = static_cast<uint8_t>(iparam(p, LfoAmd));
    d[vced::LfoSync] = static_cast<uint8_t>(iparam(p, LfoSync));
    d[vced::LfoWave] = static_cast<uint8_t>(iparam(p, LfoWave));
    d[vced::PitchModSens] = static_cast<uint8_t>(iparam(p, PitchModSens));
    d[vced::Transpose] = static_cast<uint8_t>(std::clamp(iparam(p, Transpose) + 24, 0, 48));
    for (int i = 0; i < 10; ++i) d[static_cast<size_t>(vced::Name + i)] = ' ';
    v.sanitize();
    return v;
}

std::vector<std::pair<std::string, float>> FmSynth::voiceToParams(const fm::Dx7Voice& voice) {
    fm::Dx7Voice v = voice;
    v.sanitize();
    const auto& d = v.data;
    const auto& specs = moduleInfo().params;
    std::vector<std::pair<std::string, float>> r;
    auto put = [&](int idx, int value) { r.emplace_back(specs[static_cast<size_t>(idx)].id, static_cast<float>(value)); };
    for (int n = 1; n <= 6; ++n) {
        const uint8_t* o = d.data() + vced::opBase(n);
        for (int f = 0; f < OpDetune; ++f) put(opParam(n, f), o[f]);
        put(opParam(n, OpDetune), static_cast<int>(o[vced::Detune]) - 7);
        put(opParam(n, OpOn), 1);
    }
    for (int k = 0; k < 4; ++k) {
        put(PegRate1 + k, d[static_cast<size_t>(vced::PR1 + k)]);
        put(PegLevel1 + k, d[static_cast<size_t>(vced::PL1 + k)]);
    }
    put(Alg, d[vced::Algorithm] + 1);
    put(Feedback, d[vced::Feedback]);
    put(OscSync, d[vced::OscSync]);
    put(LfoSpeed, d[vced::LfoSpeed]);
    put(LfoDelay, d[vced::LfoDelay]);
    put(LfoPmd, d[vced::LfoPmd]);
    put(LfoAmd, d[vced::LfoAmd]);
    put(LfoSync, d[vced::LfoSync]);
    put(LfoWave, d[vced::LfoWave]);
    put(PitchModSens, d[vced::PitchModSens]);
    put(Transpose, static_cast<int>(d[vced::Transpose]) - 24);
    return r;
}

void FmSynth::applyVoice(const fm::Dx7Voice& v) {
    for (const auto& [id, value] : voiceToParams(v)) params().set(id, value);
}

void FmSynth::loadState(const nlohmann::json& state) {
    state_ = state.is_object() ? state : nlohmann::json::object();
    stateError_.clear();
    voiceApplied_ = false;
    // `applied: true` = the voice was already copied into the patch params (control side); don't overwrite edits.
    if (const auto ap = state_.find("applied"); ap != state_.end() && ap->is_boolean() && ap->get<bool>()) return;
    const auto it = state_.find("syx");
    if (it == state_.end() || !it->is_string() || it->get<std::string>().empty()) return;
    const std::string path = it->get<std::string>();
    int index = 0;
    if (const auto vi = state_.find("voice"); vi != state_.end() && vi->is_number()) {
        const double d = vi->get<double>();
        index = std::isfinite(d) ? static_cast<int>(std::clamp(d, 0.0, 1000.0)) : 0;
    }

    const AppPaths paths = AppPaths::discover();
    const auto resolved = paths.resolveAllowed(path);
    if (!resolved) {
        stateError_ = "syx path outside allowed roots: " + path;
        return;
    }
    const fm::SyxParseResult r = fm::loadSyxFile(*resolved);
    if (!r.error.empty() && r.voices.empty()) {
        stateError_ = r.error;
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(r.voices.size()) - 1);
    applyVoice(r.voices[static_cast<size_t>(index)]);
    state_["applied"] = true; // the patch adopts these params (GraphBuilder -> PatchModel::adoptModuleState)
    voiceApplied_ = true;
}

} // namespace ks
