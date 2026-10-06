#include "instruments/combo/ComboOrgan.h"

#include "dsp/Math.h"
#include "dsp/PolyBlep.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {

constexpr float kDetuneCents[12] = {1.2f, -0.8f, 0.5f, -1.5f, 0.9f, -0.3f, 1.4f, -1.1f, 0.2f, -0.6f, 1.0f, -1.3f};
constexpr int kVoxFootOffset[4] = {-12, 0, 12, 24};     // 16' 8' 4' 2'
constexpr int kVoxIVOffset[4] = {19, 24, 28, 36};       // IV mixture: 2 2/3' 2' 1 3/5' 1'
// Farfisa tabs: semitone offset and bus.
constexpr int kFarOffset[7] = {-12, 0, 12, 0, 0, 0, 12};
constexpr int kFarBus[7] = {ComboOrgan::FluteBus,   ComboOrgan::FluteBus,   ComboOrgan::FluteBus,
                            ComboOrgan::OboeBus,    ComboOrgan::TrumpetBus, ComboOrgan::StringsBus,
                            ComboOrgan::StringsBus};
constexpr bool kFarStair[7] = {false, false, false, false, true, true, true};
constexpr float kFoldLimit = 0.22f; // fold sources above this fraction of the sample rate down an octave

enum Filt { FReed, FFluteA, FFluteB, FOboe, FTrumpet, FStrHp, FStrLp, FBass, FTone };

} // namespace

const ModuleInfo& ComboOrgan::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "combo";
        i.displayName = "Combo Organ";
        i.kind = ModuleKind::Instrument;
        i.category = "Organ";
        i.params = {
            enumParam("voicing", "Voicing", {"Vox Continental", "Farfisa Compact"}, 0, "Voicing"),
            intParam("vox_16", "16'", 0, 8, 0, {}, "Vox"),
            intParam("vox_8", "8'", 0, 8, 8, {}, "Vox"),
            intParam("vox_4", "4'", 0, 8, 6, {}, "Vox"),
            intParam("vox_2", "2'", 0, 8, 0, {}, "Vox"),
            intParam("vox_iv", "IV", 0, 8, 0, {}, "Vox"),
            intParam("vox_flute", "~ Flute", 0, 8, 3, {}, "Vox"),
            intParam("vox_reed", "^ Reed", 0, 8, 8, {}, "Vox"),
            boolParam("far_flute_16", "Flute 16'", false, "Farfisa"),
            boolParam("far_flute_8", "Flute 8'", true, "Farfisa"),
            boolParam("far_flute_4", "Flute 4'", false, "Farfisa"),
            boolParam("far_oboe_8", "Oboe 8'", false, "Farfisa"),
            boolParam("far_trumpet_8", "Trumpet 8'", false, "Farfisa"),
            boolParam("far_strings_8", "Strings 8'", false, "Farfisa"),
            boolParam("far_strings_4", "Strings 4'", false, "Farfisa"),
            boolParam("bass", "Bass Section", false, "Bass"),
            intParam("bass_split", "Bass Split", 36, 72, 60, {}, "Bass"),
            linearParam("bass_16", "Bass 16'", 0.0f, 1.0f, 0.8f, {}, "Bass"),
            linearParam("bass_8", "Bass 8'", 0.0f, 1.0f, 0.5f, {}, "Bass"),
            boolParam("vibrato", "Vibrato", true, "Vibrato"),
            linearParam("vibrato_rate", "Rate", 3.0f, 9.0f, 5.6f, "Hz", "Vibrato"),
            linearParam("vibrato_depth", "Depth", 0.0f, 1.0f, 0.4f, {}, "Vibrato"),
            boolParam("bright", "Bright", true, "Tone"),
            linearParam("leakage", "Leakage", 0.0f, 1.0f, 0.2f, {}, "Tone"),
            linearParam("click", "Key Click", 0.0f, 1.0f, 0.1f, {}, "Tone"),
            linearParam("velocity", "Velocity Sens", 0.0f, 1.0f, 0.0f, {}, "Tone"),
            linearParam("volume_db", "Volume", -40.0f, 6.0f, 0.0f, "dB", "Output"),
        };
        nlohmann::json controls = nlohmann::json::object();
        for (int p = Vox16; p <= VoxReed; ++p) controls[i.params[static_cast<size_t>(p)].id] = "drawbar";
        i.uiHints = {{"groupOrder", {"Voicing", "Vox", "Farfisa", "Bass", "Vibrato", "Tone", "Output"}},
                     {"front", {"voicing", "vox_16", "vox_8", "vox_4", "vox_iv", "vox_flute", "vox_reed", "vibrato",
                                "bright"}},
                     {"controls", controls}};
        return i;
    }();
    return info;
}

float ComboOrgan::masterDetuneCents(int cls) noexcept { return kDetuneCents[std::clamp(cls, 0, 11)]; }

ComboOrgan::ComboOrgan() : Module(moduleInfo()) {
    sine_.build({1.0f});
    for (int c = 0; c < kClasses; ++c) phase_[static_cast<size_t>(c)] = 0x15A4E35u * static_cast<uint32_t>(c + 1);
}

void ComboOrgan::prepare(double sampleRate, int /*maxBlock*/) {
    sampleRate_ = sampleRate;
    for (int c = 0; c < kClasses; ++c) {
        const double hz = 440.0 * std::exp2((12.0 + c - 69.0) / 12.0 + kDetuneCents[c] / 1200.0);
        baseInc_[static_cast<size_t>(c)] = hz / sampleRate * 4294967296.0;
        int o = kOctaves - 1;
        while (o > 0 && hz * std::exp2(o) > kFoldLimit * sampleRate) --o;
        maxOctave_[static_cast<size_t>(c)] = o;
        inc_[static_cast<size_t>(c)] = static_cast<uint32_t>(baseInc_[static_cast<size_t>(c)]);
    }
    for (auto& f : filt_) {
        f.setSampleRate(sampleRate);
        f.reset();
    }
    filt_[FReed].setCutoff(5200.0f, 0.15f);
    filt_[FFluteA].setCutoff(1300.0f, 0.05f);
    filt_[FFluteB].setCutoff(1900.0f, 0.0f);
    filt_[FOboe].setCutoff(1250.0f, 0.55f);
    filt_[FTrumpet].setCutoff(1500.0f, 0.4f);
    filt_[FStrHp].setCutoff(850.0f, 0.2f);
    filt_[FStrLp].setCutoff(6500.0f, 0.1f);
    filt_[FBass].setCutoff(380.0f, 0.2f);
    outGain_.prepare(sampleRate, 0.02f);
    ProcessContext ctx;
    ctx.sampleRate = sampleRate;
    brightApplied_ = -1;
    updateBlockParams(ctx);
    outGain_.snap(outGain_.target());
    reset();
}

void ComboOrgan::reset() {
    for (auto& w : wCur_) w.fill(0.0f);
    for (auto& w : wTgt_) w.fill(0.0f);
    sCur_.fill(0.0f);
    sTgt_.fill(0.0f);
    down_.fill(false);
    gate_.fill(0.0f);
    for (auto& f : filt_) f.reset();
    activeKeys_.store(0, std::memory_order_relaxed);
}

void ComboOrgan::updateBlockParams(const ProcessContext& ctx) noexcept {
    const ParamSet& p = params();
    vox_ = p.get(Voicing) < 0.5f;
    auto bar = [&](int idx) {
        const float v = p.get(idx);
        return v < 0.5f ? 0.0f : std::exp2((v - 8.0f) * 0.5f);
    };
    for (int f = 0; f < 4; ++f) voxFoot_[f] = bar(Vox16 + f);
    voxIV_ = bar(VoxIV);
    voxFlute_ = bar(VoxFlute);
    voxReed_ = bar(VoxReed);
    for (int t = 0; t < 7; ++t) far_[t] = p.get(FarFlute16 + t) >= 0.5f;
    bass_ = p.get(Bass) >= 0.5f;
    bassSplit_ = static_cast<int>(p.get(BassSplit));
    bass16_ = p.get(Bass16);
    bass8_ = p.get(Bass8);
    vibDepthSemis_ = p.get(VibratoOn) >= 0.5f ? 0.5f * p.get(VibratoDepth) : 0.0f;
    vibInc_ = p.get(VibratoRate) / static_cast<float>(sampleRate_);
    leak_ = p.get(Leakage);
    velSens_ = p.get(Velocity);
    const float click = p.get(Click);
    const float sr = static_cast<float>(sampleRate_);
    attackStep_ = 1.0f / std::max(1.0f, (0.004f - 0.0037f * click) * sr);
    releaseStep_ = 1.0f / std::max(1.0f, (0.006f - 0.005f * click) * sr);
    bright_ = p.get(Bright) >= 0.5f;
    if (static_cast<int>(bright_) != brightApplied_) {
        filt_[FTone].setCutoff(bright_ ? 9000.0f : 2600.0f, 0.0f);
        brightApplied_ = static_cast<int>(bright_);
    }
    const float expr = ctx.channel ? std::clamp(ctx.channel->expression, 0.0f, 1.0f) : 1.0f;
    const float swell = expr <= 0.0f ? 0.0f : dsp::dbToGain(-30.0f * (1.0f - expr));
    outGain_.setTarget(0.09f * dsp::dbToGain(p.get(VolumeDb)) * swell);
}

int ComboOrgan::sourceFor(int pitch) const noexcept {
    if (pitch < 12) pitch += 12 * ((12 - pitch + 11) / 12);
    const int c = pitch % 12;
    int o = pitch / 12 - 1;
    o = std::min(o, maxOctave_[static_cast<size_t>(c)]);
    return o < 0 ? -1 : c * kOctaves + o;
}

void ComboOrgan::addPitch(int pitch, int bus, float w) noexcept {
    const int s = sourceFor(pitch);
    if (s >= 0) wTgt_[static_cast<size_t>(s)][static_cast<size_t>(bus)] += w;
}

void ComboOrgan::addSine(int pitch, float w) noexcept {
    const int s = sourceFor(pitch);
    if (s >= 0) sTgt_[static_cast<size_t>(s)] += w;
}

void ComboOrgan::addStair(int pitch, int bus, float w) noexcept {
    // Divider staircase: square + 1/2 octave-up square + 1/4 two-up + 1/8 three-up ~ band-limited ramp.
    constexpr float kNorm = 1.0f / 1.875f;
    float a = w * kNorm;
    for (int j = 0; j < 4; ++j, a *= 0.5f) addPitch(pitch + 12 * j, bus, a);
}

void ComboOrgan::handleEvent(const MidiEvent& e) noexcept {
    switch (e.type) {
    case MidiEventType::NoteOn:
        down_[e.data1 & 127] = true;
        vel_[e.data1 & 127] = 1.0f - velSens_ * (1.0f - e.valueF);
        break;
    case MidiEventType::NoteOff: down_[e.data1 & 127] = false; break;
    case MidiEventType::AllNotesOff: down_.fill(false); break;
    case MidiEventType::AllSoundOff: reset(); break;
    default: break;
    }
}

void ComboOrgan::renderChunk(float* dst, int len) noexcept {
    // 1. Key gates -> source weights.
    for (auto& w : wTgt_) w.fill(0.0f);
    sTgt_.fill(0.0f);
    int active = 0;
    float gateSum = 0.0f;
    const float fl = static_cast<float>(len);
    for (int n = 0; n < 128; ++n) {
        float& g = gate_[static_cast<size_t>(n)];
        const bool d = down_[static_cast<size_t>(n)];
        if (!d && g <= 0.0f) continue;
        g = d ? std::min(1.0f, g + attackStep_ * fl) : std::max(0.0f, g - releaseStep_ * fl);
        if (g <= 0.0f) continue;
        ++active;
        const float gv = g * vel_[static_cast<size_t>(n)];
        gateSum += gv;
        if (bass_ && n < bassSplit_) {
            addPitch(n - 12, BassBus, gv * bass16_);
            addPitch(n, BassBus, gv * bass8_);
        } else if (vox_) {
            for (int f = 0; f < 4; ++f) {
                if (voxFoot_[f] <= 0.0f) continue;
                const int p = n + kVoxFootOffset[f];
                addSine(p, gv * voxFoot_[f] * voxFlute_);
                addStair(p, VoxReedBus, gv * voxFoot_[f] * voxReed_);
            }
            if (voxIV_ > 0.0f)
                for (int r = 0; r < 4; ++r) {
                    const int p = n + kVoxIVOffset[r];
                    addSine(p, gv * voxIV_ * voxFlute_ * 0.55f);
                    addStair(p, VoxReedBus, gv * voxIV_ * voxReed_ * 0.55f);
                }
        } else {
            for (int t = 0; t < 7; ++t) {
                if (!far_[t]) continue;
                const int p = n + kFarOffset[t];
                if (kFarStair[t]) addStair(p, kFarBus[t], gv);
                else addPitch(p, kFarBus[t], gv);
            }
        }
    }
    activeKeys_.store(active, std::memory_order_relaxed);
    // Leakage: faint bleed of every master's top divider stages while the organ is being played.
    if (leak_ > 0.0f && gateSum > 0.0f) {
        const float lw = leak_ * 0.004f * std::min(1.0f, gateSum);
        const int bus = vox_ ? VoxReedBus : FluteBus;
        for (int c = 0; c < kClasses; ++c) {
            const int o = std::min(6, maxOctave_[static_cast<size_t>(c)]);
            wTgt_[static_cast<size_t>(c * kOctaves + o)][static_cast<size_t>(bus)] += lw;
        }
    }

    // 2. Vibrato on the master oscillators (all dividers follow).
    float vib = 0.0f;
    if (vibDepthSemis_ > 0.0f) {
        vibPhase_ += vibInc_ * fl;
        if (vibPhase_ >= 1.0f) vibPhase_ -= std::floor(vibPhase_);
        vib = vibDepthSemis_ * std::sin(dsp::kTwoPi * vibPhase_);
    }
    const double ratio = std::exp2(static_cast<double>(vib) / 12.0);
    for (int c = 0; c < kClasses; ++c)
        inc_[static_cast<size_t>(c)] = static_cast<uint32_t>(baseInc_[static_cast<size_t>(c)] * ratio);

    // 3. Sources -> busses.
    float bus[kSqBusses + 1][kChunk] = {};
    float sq[kChunk];
    const float invLen = 1.0f / fl;
    constexpr float k2_32 = 1.0f / 4294967296.0f;
    for (int c = 0; c < kClasses; ++c) {
        const uint32_t ph0 = phase_[static_cast<size_t>(c)];
        const uint32_t inc0 = inc_[static_cast<size_t>(c)];
        for (int o = 0; o < kOctaves; ++o) {
            const size_t s = static_cast<size_t>(c * kOctaves + o);
            auto& wc = wCur_[s];
            const auto& wt = wTgt_[s];
            bool any = sCur_[s] != 0.0f || sTgt_[s] != 0.0f;
            bool anySq = false;
            for (int b = 0; b < kSqBusses; ++b) anySq |= (wc[static_cast<size_t>(b)] != 0.0f || wt[static_cast<size_t>(b)] != 0.0f);
            if (!any && !anySq) continue;
            const uint32_t inc = inc0 << o;
            const float dt = static_cast<float>(inc) * k2_32;
            uint32_t ph = ph0 << o;
            if (anySq) {
                uint32_t p2 = ph;
                for (int i = 0; i < len; ++i) {
                    const float t = static_cast<float>(p2) * k2_32;
                    const float th = static_cast<float>(p2 + 0x80000000u) * k2_32;
                    sq[i] = (t < 0.5f ? 1.0f : -1.0f) + dsp::PolyBlepOsc::blep(t, dt) - dsp::PolyBlepOsc::blep(th, dt);
                    p2 += inc;
                }
                for (int b = 0; b < kSqBusses; ++b) {
                    float w = wc[static_cast<size_t>(b)];
                    const float tg = wt[static_cast<size_t>(b)];
                    if (w == 0.0f && tg == 0.0f) continue;
                    const float dw = (tg - w) * invLen;
                    for (int i = 0; i < len; ++i) {
                        w += dw;
                        bus[b][i] += w * sq[i];
                    }
                    wc[static_cast<size_t>(b)] = tg;
                }
            }
            if (any) {
                float w = sCur_[s];
                const float dw = (sTgt_[s] - w) * invLen;
                uint32_t p2 = ph;
                for (int i = 0; i < len; ++i) {
                    w += dw;
                    bus[kSqBusses][i] += w * sine_.lookup(p2);
                    p2 += inc;
                }
                sCur_[s] = sTgt_[s];
            }
        }
        phase_[static_cast<size_t>(c)] = ph0 + inc0 * static_cast<uint32_t>(len);
    }

    // 4. Voice formants, sum.
    for (int i = 0; i < len; ++i) {
        float y = bus[kSqBusses][i]; // Vox flute (sines), unfiltered
        y += filt_[FReed].tick(bus[VoxReedBus][i]).lp;
        y += 1.3f * filt_[FFluteB].tick(filt_[FFluteA].tick(bus[FluteBus][i]).lp).lp;
        const auto ob = filt_[FOboe].tick(bus[OboeBus][i]);
        y += 1.6f * ob.bp + 0.25f * ob.lp;
        const auto tr = filt_[FTrumpet].tick(bus[TrumpetBus][i]);
        y += 1.3f * tr.bp + 0.45f * bus[TrumpetBus][i];
        y += 0.9f * filt_[FStrLp].tick(filt_[FStrHp].tick(bus[StringsBus][i]).hp).lp;
        y += 1.2f * filt_[FBass].tick(bus[BassBus][i]).lp;
        dst[i] = filt_[FTone].tick(y).lp;
    }
}

void ComboOrgan::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    updateBlockParams(ctx);
    float* buf = out.left;
    int pos = 0;
    size_t ev = 0;
    while (pos < out.numSamples) {
        while (ev < events.size() && static_cast<int>(events[ev].sampleOffset) <= pos) handleEvent(events[ev++]);
        int end = std::min(out.numSamples, pos + kChunk);
        if (ev < events.size()) end = std::min(end, std::max(pos + 1, static_cast<int>(events[ev].sampleOffset)));
        renderChunk(buf + pos, end - pos);
        pos = end;
    }
    while (ev < events.size()) handleEvent(events[ev++]);
    for (int i = 0; i < out.numSamples; ++i) buf[i] *= outGain_.next();
    std::copy(out.left, out.left + out.numSamples, out.right);
}

} // namespace ks
