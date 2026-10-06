#include "instruments/basic/BasicSynth.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
constexpr int kControlInterval = 16; // samples between pitch/filter updates
}

const ModuleInfo& BasicSynth::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "basic";
        i.displayName = "Basic Synth";
        i.kind = ModuleKind::Instrument;
        i.category = "Synth";
        i.params = {
            enumParam("wave", "Wave", {"Saw", "Square"}, 0, "Osc"),
            logParam("cutoff", "Cutoff", 20.0f, 20000.0f, 3000.0f, "Hz", "Filter", 1000.0f),
            linearParam("resonance", "Resonance", 0.0f, 1.0f, 0.2f, {}, "Filter"),
            logParam("attack", "Attack", 0.001f, 5.0f, 0.005f, "s", "Amp", 0.1f),
            logParam("decay", "Decay", 0.001f, 5.0f, 0.3f, "s", "Amp", 0.3f),
            linearParam("sustain", "Sustain", 0.0f, 1.0f, 0.7f, {}, "Amp"),
            logParam("release", "Release", 0.001f, 10.0f, 0.3f, "s", "Amp", 0.5f),
            linearParam("volume_db", "Volume", -60.0f, 6.0f, -6.0f, "dB", "Amp"),
            enumParam("voice_mode", "Voice Mode", {"Poly", "Mono", "Legato"}, 0, "Voice"),
            linearParam("glide", "Glide", 0.0f, 2.0f, 0.0f, "s", "Voice"),
            intParam("polyphony", "Polyphony", 1, kMaxVoices, kMaxVoices, {}, "Voice"),
        };
        i.uiHints = {{"groupOrder", {"Osc", "Filter", "Amp", "Voice"}},
                     {"front", {"wave", "cutoff", "resonance", "attack", "release", "volume_db"}}};
        return i;
    }();
    return info;
}

BasicSynth::BasicSynth() : Module(moduleInfo()) {
    for (int i = 0; i < kMaxVoices; ++i) alloc_.voice(i).shared = &shared_;
}

void BasicSynth::prepare(double sampleRate, int /*maxBlock*/) {
    shared_.sampleRate = sampleRate;
    for (int i = 0; i < kMaxVoices; ++i) {
        auto& v = alloc_.voice(i);
        v.osc.setSampleRate(sampleRate);
        v.svf.setSampleRate(sampleRate);
        v.env.setSampleRate(sampleRate);
    }
    ProcessContext ctx;
    ctx.sampleRate = sampleRate;
    updateShared(ctx);
    reset();
}

void BasicSynth::reset() {
    alloc_.reset();
    activeVoices_.store(0, std::memory_order_relaxed);
}

int BasicSynth::tailSamples() const {
    return static_cast<int>(params().get(Release) * static_cast<float>(shared_.sampleRate)) + 64;
}

void BasicSynth::updateShared(const ProcessContext& ctx) noexcept {
    const ParamSet& p = params();
    shared_.wave = p.get(Wave) >= 0.5f ? dsp::Wave::Square : dsp::Wave::Saw;
    shared_.cutoff = p.get(Cutoff);
    shared_.resonance = p.get(Resonance);
    shared_.env.attack = p.get(Attack);
    shared_.env.decay = p.get(Decay);
    shared_.env.sustain = p.get(Sustain);
    shared_.env.release = p.get(Release);
    shared_.gain = 0.25f * dsp::dbToGain(p.get(VolumeDb));
    const float glide = p.get(Glide);
    shared_.glideCoef = glide <= 0.0005f
                            ? 1.0f
                            : dsp::onePoleCoef(glide / 3.0f, shared_.sampleRate / kControlInterval);
    shared_.bendSemis = ctx.channel ? ctx.channel->pitchBend * 2.0f : 0.0f;

    const auto mode = static_cast<VoiceMode>(std::clamp(static_cast<int>(p.get(VoiceModeP)), 0, 2));
    if (mode != mode_) {
        mode_ = mode;
        alloc_.setMode(mode);
    }
    alloc_.setPolyphony(static_cast<int>(p.get(Polyphony)));
    for (int i = 0; i < kMaxVoices; ++i) alloc_.voice(i).env.setParams(shared_.env);
}

void BasicSynth::Voice::noteOn(const VoiceStart& s) noexcept {
    const bool wasActive = env.isActive();
    targetNote = static_cast<float>(s.note);
    const bool glide = shared->glideCoef < 1.0f && s.glideFrom >= 0;
    if (glide) {
        if (!wasActive) note = static_cast<float>(s.glideFrom);
    } else {
        note = targetNote;
    }
    if (!s.legato) {
        velGain = 0.35f + 0.65f * s.velocity;
        env.noteOn();
    }
    if (!wasActive) {
        osc.resetPhase();
        svf.reset();
    }
    tick = 0;
}

void BasicSynth::Voice::reset() noexcept {
    env.reset();
    svf.reset();
    osc.resetPhase();
}

void BasicSynth::Voice::render(float* out, int n) noexcept {
    const Shared& sh = *shared;
    for (int i = 0; i < n; ++i) {
        if (tick-- <= 0) {
            tick = kControlInterval - 1;
            note += (targetNote - note) * sh.glideCoef;
            osc.setFrequency(dsp::noteToHz(note + sh.bendSemis));
            svf.setCutoff(sh.cutoff, sh.resonance);
        }
        const float x = osc.next(sh.wave);
        const float y = svf.tick(x).lp;
        out[i] += y * env.next() * velGain * sh.gain;
        if (!env.isActive()) break;
    }
}

void BasicSynth::renderSegment(AudioBlock& out, int start, int end) noexcept {
    if (end <= start) return;
    alloc_.forEachActive([&](Voice& v) { v.render(out.left + start, end - start); });
}

void BasicSynth::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    updateShared(ctx);
    alloc_.setSustainEnabled(true);
    int pos = 0;
    for (const MidiEvent& e : events) {
        const int at = std::clamp(static_cast<int>(e.sampleOffset), pos, out.numSamples);
        renderSegment(out, pos, at);
        pos = at;
        alloc_.handleEvent(e);
    }
    renderSegment(out, pos, out.numSamples);
    std::copy(out.left, out.left + out.numSamples, out.right);
    activeVoices_.store(alloc_.activeCount(), std::memory_order_relaxed);
}

} // namespace ks
