#include "instruments/drums/DrumKit.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

using drums::Instr;
using drums::Model;

namespace {

constexpr float kOutputGain = 0.7f; // headroom: a full kit at velocity 127 peaks around -3 dBFS

struct VoiceDef {
    const char* id;
    const char* group;
    float levelDb;
    float pan;
};

// Order = drums::Instr.
constexpr VoiceDef kVoices[drums::kNumInstr] = {
    {"kick", "Kick", 0.0f, 0.0f},          {"snare", "Snare", -1.0f, 0.0f},
    {"clap", "Clap", -2.0f, 0.05f},        {"chh", "Closed Hat", -4.0f, 0.25f},
    {"ohh", "Open Hat", -5.0f, 0.25f},     {"crash", "Crash", -6.0f, -0.3f},
    {"ride", "Ride", -7.0f, 0.35f},        {"tom_lo", "Low Tom", -2.0f, -0.35f},
    {"tom_mid", "Mid Tom", -2.0f, 0.0f},   {"tom_hi", "High Tom", -2.0f, 0.35f},
    {"rim", "Rim", -4.0f, -0.1f},          {"cowbell", "Cowbell", -6.0f, 0.15f},
    {"tamb", "Tambourine", -6.0f, -0.2f},
};

} // namespace

const ModuleInfo& DrumKit::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "drums";
        i.displayName = "Drum Machine";
        i.kind = ModuleKind::Instrument;
        i.category = "Drums";
        i.params = {
            enumParam("kit", "Kit", {"TR-808", "TR-909", "Linn", "Industrial"}, 0, "Kit"),
            linearParam("volume_db", "Volume", -40.0f, 6.0f, 0.0f, "dB", "Kit"),
            linearParam("velocity", "Velocity Sens", 0.0f, 1.0f, 0.8f, {}, "Kit"),
        };
        nlohmann::json groupOrder = nlohmann::json::array({"Kit"});
        for (const VoiceDef& v : kVoices) {
            const std::string p = v.id, g = v.group;
            i.params.push_back(enumParam(p + "_model", "Model", {"Kit", "808", "909", "Linn", "Industrial"}, 0, g));
            i.params.push_back(linearParam(p + "_level", "Level", -40.0f, 6.0f, v.levelDb, "dB", g));
            i.params.push_back(linearParam(p + "_tune", "Tune", -12.0f, 12.0f, 0.0f, "st", g));
            i.params.push_back(linearParam(p + "_decay", "Decay", 0.0f, 1.0f, 0.5f, {}, g));
            i.params.push_back(linearParam(p + "_tone", "Tone", 0.0f, 1.0f, 0.5f, {}, g));
            i.params.push_back(linearParam(p + "_pan", "Pan", -1.0f, 1.0f, v.pan, {}, g));
            groupOrder.push_back(g);
        }
        i.uiHints = {{"groupOrder", groupOrder},
                     {"front", {"kit", "volume_db", "kick_level", "snare_level", "chh_level", "kick_tune", "kick_decay"}},
                     {"tabs",
                      {{{"name", "Kit"}, {"groups", {"Kit"}}},
                       {{"name", "Kick / Snare"}, {"groups", {"Kick", "Snare", "Clap", "Rim"}}},
                       {{"name", "Hats / Cymbals"}, {"groups", {"Closed Hat", "Open Hat", "Crash", "Ride"}}},
                       {{"name", "Toms"}, {"groups", {"Low Tom", "Mid Tom", "High Tom"}}},
                       {{"name", "Perc"}, {"groups", {"Cowbell", "Tambourine"}}}}}};
        return i;
    }();
    return info;
}

DrumKit::DrumKit() : Module(moduleInfo()) {}

void DrumKit::prepare(double sampleRate, int /*maxBlock*/) {
    sampleRate_ = sampleRate;
    for (auto& v : voices_) v.setSampleRate(sampleRate);
    volume_.prepare(sampleRate, 0.01f);
    dcR_ = 1.0f - static_cast<float>(2.0 * 3.14159265358979 * 8.0 / sampleRate); // ~8 Hz
    volume_.snap(kOutputGain * dsp::dbToGain(params().get(VolumeDb)));
    reset();
}

void DrumKit::reset() {
    for (auto& v : voices_) v.kill();
    dcX_[0] = dcX_[1] = dcY_[0] = dcY_[1] = 0.0f;
    activeVoices_.store(0, std::memory_order_relaxed);
}

DrumKit::NoteMapping DrumKit::mapNote(int note) noexcept {
    NoteMapping m;
    auto set = [&](Instr i, float tune = 0.0f, float level = 1.0f, float decay = 1.0f) {
        m.instr = i;
        m.tuneSt = tune;
        m.level = level;
        m.decayMul = decay;
    };
    switch (note) {
    case 35: set(Instr::Kick, -2.0f); return m;
    case 36: set(Instr::Kick); return m;
    case 37: set(Instr::Rim); return m;
    case 38: set(Instr::Snare); return m;
    case 39: set(Instr::Clap); return m;
    case 40: set(Instr::Snare, 1.0f, 1.0f, 1.2f); return m; // electric snare: a bit brighter/longer
    case 41: set(Instr::TomLo, -4.0f, 1.0f, 1.2f); return m;
    case 42: set(Instr::ClosedHat); return m;
    case 43: set(Instr::TomLo, -2.0f, 1.0f, 1.1f); return m;
    case 44: set(Instr::ClosedHat, 0.0f, 0.6f, 0.6f); return m; // pedal hat
    case 45: set(Instr::TomLo); return m;
    case 46: set(Instr::OpenHat); return m;
    case 47: set(Instr::TomMid); return m;
    case 48: set(Instr::TomMid, 2.0f); return m;
    case 49: set(Instr::Crash); return m;
    case 50: set(Instr::TomHi); return m;
    case 51: set(Instr::Ride); return m;
    case 52: set(Instr::Crash, -3.0f, 0.9f, 1.1f); return m; // china-ish
    case 53: set(Instr::Ride); m.rideBell = true; return m;
    case 54: set(Instr::Tamb); return m;
    case 55: set(Instr::Crash, 3.0f, 0.8f, 0.6f); return m; // splash
    case 56: set(Instr::Cowbell); return m;
    case 57: set(Instr::Crash, 1.0f); return m;
    case 58: set(Instr::Cowbell, 5.0f, 0.8f, 0.7f); return m;
    case 59: set(Instr::Ride, 1.0f); return m;
    case 69: set(Instr::Tamb, 2.0f, 0.7f, 0.6f); return m; // cabasa
    case 70: set(Instr::Tamb, 4.0f, 0.7f, 0.4f); return m; // maracas
    default: break;
    }
    // Fold every other key onto 36..47 by pitch class.
    const int folded = 36 + (((note - 36) % 12) + 12) % 12;
    return mapNote(folded);
}

void DrumKit::trigger(int note, float velocity) noexcept {
    const NoteMapping m = mapNote(note);
    const int ii = static_cast<int>(m.instr);
    const ParamSet& p = params();
    const int base = kVoiceBase + ii * kPerVoice;
    int modelSel = static_cast<int>(p.get(base + VModel));
    const int model = modelSel <= 0 ? static_cast<int>(p.get(Kit)) : modelSel - 1;
    const float tune = p.get(base + VTune) + m.tuneSt;
    const float decay = p.get(base + VDecay);
    const float ds = std::exp2((decay - 0.5f) * 3.0f) * m.decayMul; // 0.35x .. 2.8x
    const float tone = p.get(base + VTone);
    const float sens = p.get(VelocitySens);
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    const float velGain = (1.0f - sens) + sens * std::pow(v, 1.5f);
    const float amp = velGain * m.level * dsp::dbToGain(p.get(base + VLevel));
    float gl = 1.0f, gr = 1.0f;
    dsp::panGains(p.get(base + VPan), gl, gr);
    gl *= 1.41421356f;
    gr *= 1.41421356f;

    const drums::Recipe r = drums::makeRecipe(m.instr, static_cast<Model>(std::clamp(model, 0, 3)), tune, ds, tone, v,
                                              m.rideBell);

    // Choke: closed / pedal hat stops the open hat.
    auto chokeAll = [&](Instr i) {
        for (int s = 0; s < kSlotsPerInstr; ++s)
            voices_[static_cast<size_t>(static_cast<int>(i) * kSlotsPerInstr + s)].choke();
    };
    if (m.instr == Instr::ClosedHat) chokeAll(Instr::OpenHat);

    // Retrigger: fade the previous hit(s) of this instrument, take a free slot (else the oldest).
    drums::DrumVoice* target = nullptr;
    drums::DrumVoice* oldest = nullptr;
    for (int s = 0; s < kSlotsPerInstr; ++s) {
        auto& vs = voices_[static_cast<size_t>(ii * kSlotsPerInstr + s)];
        if (!vs.active()) {
            if (!target) target = &vs;
            continue;
        }
        if (!vs.choking()) vs.choke();
        if (!oldest || vs.age() > oldest->age()) oldest = &vs;
    }
    if (!target) target = oldest;
    seed_ = seed_ * 1664525u + 1013904223u;
    target->trigger(r, amp, gl, gr, seed_);
}

void DrumKit::renderSegment(AudioBlock& out, int start, int end) noexcept {
    if (end <= start) return;
    for (auto& v : voices_)
        if (v.active()) v.render(out.left + start, out.right + start, end - start);
}

void DrumKit::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& /*ctx*/) {
    int pos = 0;
    for (const MidiEvent& e : events) {
        const int at = std::clamp(static_cast<int>(e.sampleOffset), pos, out.numSamples);
        renderSegment(out, pos, at);
        pos = at;
        if (e.type == MidiEventType::NoteOn && e.value7 > 0) trigger(e.data1, static_cast<float>(e.value7) / 127.0f);
        else if (e.type == MidiEventType::AllSoundOff) reset();
    }
    renderSegment(out, pos, out.numSamples);
    volume_.setTarget(kOutputGain * dsp::dbToGain(params().get(VolumeDb)));
    for (int i = 0; i < out.numSamples; ++i) {
        const float g = volume_.next();
        const float l = out.left[i], r = out.right[i];
        dcY_[0] = l - dcX_[0] + dcR_ * dcY_[0];
        dcX_[0] = l;
        dcY_[1] = r - dcX_[1] + dcR_ * dcY_[1];
        dcX_[1] = r;
        out.left[i] = dcY_[0] * g;
        out.right[i] = dcY_[1] * g;
    }
    if (std::fabs(dcY_[0]) < 1e-15f) dcY_[0] = 0.0f; // denormal guard (FTZ is on anyway)
    if (std::fabs(dcY_[1]) < 1e-15f) dcY_[1] = 0.0f;
    int active = 0;
    for (const auto& v : voices_) active += v.active() ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

} // namespace ks
