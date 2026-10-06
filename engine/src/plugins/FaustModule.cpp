#include "plugins/FaustModule.h"

#include "plugins/FpuGuard.h"
#include "platform/DynamicLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ks::plugins {

namespace {
constexpr float kSilence = 3.2e-5f;      // -90 dBFS
constexpr int kSilentSamplesToStop = 2048;

float peakOf(const float* p, int n) noexcept {
    float m = 0.0f;
    for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(p[i]));
    return m; // NaN is skipped here: check finiteBlock() separately
}
bool finiteBlock(const float* p, int n) noexcept {
    float acc = 0.0f;
    for (int i = 0; i < n; ++i) acc += p[i] * 0.0f; // NaN/Inf * 0 = NaN
    return acc == 0.0f;
}
} // namespace

std::unique_ptr<Module> FaustPluginVersion::createModule() const {
    auto self = std::static_pointer_cast<const FaustPluginVersion>(shared_from_this());
    std::unique_ptr<FaustModuleBase> m;
    if (instrument) m = std::make_unique<FaustInstrument>(self);
    else m = std::make_unique<FaustEffect>(self);
    return m;
}

// --- base ----------------------------------------------------------------------------------------------------

FaustModuleBase::FaustModuleBase(std::shared_ptr<const FaustPluginVersion> v) : Module(v->info), v_(std::move(v)) {}

FaustModuleBase::~FaustModuleBase() {
    for (auto& i : instances_) faust::deleteInstance(i.dsp);
    instances_.clear();
}

bool FaustModuleBase::makeInstance(FaustInstance& inst) {
    inst.dsp = faust::createInstance(*v_->factory);
    if (!inst.dsp) return false;
    faust::init(inst.dsp, static_cast<int>(std::lround(sampleRate_)));
    inst.zones = faust::zones(inst.dsp);
    const auto& b = v_->mapping.bindings;
    if (inst.zones.size() != b.size()) return false;
    inst.params.assign(static_cast<size_t>(params().size()), nullptr);
    for (size_t i = 0; i < b.size(); ++i) {
        float* z = inst.zones[i];
        if (b[i].paramIndex >= 0) {
            inst.params[static_cast<size_t>(b[i].paramIndex)] = z;
            continue;
        }
        switch (b[i].role) {
        case VoiceRole::Freq: inst.freq = z; break;
        case VoiceRole::Key: inst.key = z; break;
        case VoiceRole::Gain: inst.gain = z; break;
        case VoiceRole::Velocity: inst.vel = z; break;
        case VoiceRole::Gate: inst.gate = z; break;
        case VoiceRole::None: break;
        }
    }
    // Resolve every JIT entry point used on the audio thread once here (first calls may page in code).
    float dummyL = 0.0f, dummyR = 0.0f;
    float* outs[2] = {&dummyL, &dummyR};
    float in0 = 0.0f, in1 = 0.0f;
    float* ins[2] = {&in0, &in1};
    faust::compute(inst.dsp, 0, ins, outs);
    faust::clear(inst.dsp);
    return true;
}

void FaustModuleBase::pushParams(FaustInstance& inst) noexcept {
    const auto& specs = params().specs();
    for (int i = 0; i < params().size(); ++i) {
        float* z = inst.params[static_cast<size_t>(i)];
        if (!z || specs[static_cast<size_t>(i)].isReadOnly()) continue;
        float v = params().get(i);
        const auto& ev = enumValues_[static_cast<size_t>(i)];
        if (ev) {
            const int idx = std::clamp(static_cast<int>(std::lround(v)), 0, static_cast<int>(ev->size()) - 1);
            v = (*ev)[static_cast<size_t>(idx)];
        }
        *z = v;
    }
}

void FaustModuleBase::pullReadOnly(const FaustInstance& inst) noexcept {
    const auto& specs = params().specs();
    for (int i = 0; i < params().size(); ++i)
        if (specs[static_cast<size_t>(i)].isReadOnly())
            if (const float* z = inst.params[static_cast<size_t>(i)]) params().setRaw(i, *z);
}

void FaustModuleBase::fault(uint32_t code) noexcept {
    faulted_ = true;
    v_->reportFault(code);
}

void FaustModuleBase::bindEnums() {
    enumValues_.assign(static_cast<size_t>(params().size()), nullptr);
    for (const auto& b : v_->mapping.bindings)
        if (b.paramIndex >= 0 && !b.enumValues.empty()) enumValues_[static_cast<size_t>(b.paramIndex)] = &b.enumValues;
}

// --- instrument ----------------------------------------------------------------------------------------------

FaustInstrument::FaustInstrument(std::shared_ptr<const FaustPluginVersion> v) : FaustModuleBase(std::move(v)) {
    bindEnums();
    for (int i = 0; i < kMaxVoices; ++i) alloc_.voice(i).shared = &shared_;
}

void FaustInstrument::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
    for (auto& i : instances_) faust::deleteInstance(i.dsp);
    instances_.clear();
    const int poly = std::clamp(v_->polyphony, 1, kMaxVoices);
    instances_.resize(static_cast<size_t>(poly)); // never resized again: voices keep pointers
    valid_ = true;
    for (auto& inst : instances_) valid_ = valid_ && makeInstance(inst);
    for (int i = 0; i < kMaxVoices; ++i) alloc_.voice(i).inst = i < poly && valid_ ? &instances_[static_cast<size_t>(i)] : nullptr;
    alloc_.setPolyphony(poly);
    shared_.l.assign(static_cast<size_t>(maxBlock) + 1, 0.0f);
    shared_.r.assign(static_cast<size_t>(maxBlock) + 1, 0.0f);
    shared_.stereo = v_->numOutputs >= 2;
    shared_.maxReleaseSamples = static_cast<int64_t>(std::max(0.05f, v_->maxReleaseSeconds) * sampleRate);
    faulted_ = false;
    reset();
}

void FaustInstrument::reset() {
    alloc_.reset();
    active_.store(0, std::memory_order_relaxed);
}

int FaustInstrument::tailSamples() const {
    return static_cast<int>(std::min(2.0f, v_->maxReleaseSeconds) * static_cast<float>(sampleRate_));
}

void FaustInstrument::Voice::setPitch(float bendSemis) noexcept {
    if (!inst) return;
    if (inst->freq) *inst->freq = 440.0f * std::exp2((static_cast<float>(note) + bendSemis - 69.0f) / 12.0f);
    if (inst->key) *inst->key = static_cast<float>(note);
}

void FaustInstrument::Voice::noteOn(const VoiceStart& s) noexcept {
    if (!inst) return;
    note = s.note;
    if (inst->gain) *inst->gain = s.velocity;
    if (inst->vel) *inst->vel = s.velocity * 127.0f;
    setPitch(shared->bendSemis);
    retrigger = active; // re-strike / steal: gate goes 0 for one sample so envelopes retrigger
    if (inst->gate) *inst->gate = retrigger ? 0.0f : 1.0f;
    active = true;
    released = false;
    silentSamples = 0;
    releasedSamples = 0;
}

void FaustInstrument::Voice::noteOff() noexcept {
    if (!inst || !active) return;
    if (inst->gate) *inst->gate = 0.0f;
    retrigger = false;
    released = true;
}

void FaustInstrument::Voice::kill() noexcept {} // the following noteOn retriggers (gate 0 -> 1)

void FaustInstrument::Voice::reset() noexcept {
    if (inst) {
        if (inst->gate) *inst->gate = 0.0f;
        faust::clear(inst->dsp);
    }
    active = released = retrigger = false;
    silentSamples = 0;
    releasedSamples = 0;
}

void FaustInstrument::Voice::render(AudioBlock& out, int start, int n) noexcept {
    if (n <= 0 || !inst) return;
    Shared& sh = *shared;
    float* outs[2] = {sh.l.data(), sh.r.data()};
    float* ins[2] = {sh.l.data(), sh.r.data()}; // instruments have no inputs; valid pointers anyway
    int off = 0;
    if (retrigger) {
        faust::compute(inst->dsp, 1, ins, outs);
        if (inst->gate) *inst->gate = 1.0f;
        retrigger = false;
        off = 1;
    }
    if (n > off) {
        float* o2[2] = {sh.l.data() + off, sh.r.data() + off};
        faust::compute(inst->dsp, n - off, ins, o2);
    }
    if (!finiteBlock(sh.l.data(), n) || (sh.stereo && !finiteBlock(sh.r.data(), n))) {
        // Plugin bug: drop this voice's block, clear its state, stop it.
        reset();
        return;
    }
    float* L = out.left + start;
    float* R = out.right + start;
    if (sh.stereo) {
        for (int i = 0; i < n; ++i) {
            L[i] += sh.l[static_cast<size_t>(i)];
            R[i] += sh.r[static_cast<size_t>(i)];
        }
    } else {
        for (int i = 0; i < n; ++i) L[i] += sh.l[static_cast<size_t>(i)];
    }
    if (released) {
        const float pk = std::max(peakOf(sh.l.data(), n), sh.stereo ? peakOf(sh.r.data(), n) : 0.0f);
        releasedSamples += n;
        silentSamples = pk < kSilence ? silentSamples + n : 0;
        if (silentSamples >= kSilentSamplesToStop || releasedSamples >= sh.maxReleaseSamples) {
            if (releasedSamples >= sh.maxReleaseSamples) faust::clear(inst->dsp); // cut a drone cleanly
            active = false;
            released = false;
        }
    }
}

void FaustInstrument::guardedProcess(void* self) { static_cast<FaustInstrument*>(self)->processBody(); }

void FaustInstrument::processBody() noexcept {
    AudioBlock& out = *curOut_;
    const float bend = curCtx_->channel ? curCtx_->channel->pitchBend * 2.0f : 0.0f;
    const bool bendChanged = bend != shared_.bendSemis;
    shared_.bendSemis = bend;
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = alloc_.voice(i);
        if (!v.inst) continue;
        pushParams(*v.inst);
        if (bendChanged && v.active) v.setPitch(bend);
    }
    alloc_.setSustainEnabled(true);
    int pos = 0;
    for (const MidiEvent& e : curEvents_) {
        const int at = std::clamp(static_cast<int>(e.sampleOffset), pos, out.numSamples);
        if (at > pos) alloc_.forEachActive([&](Voice& v) { v.render(out, pos, at - pos); });
        pos = at;
        alloc_.handleEvent(e);
    }
    if (out.numSamples > pos) alloc_.forEachActive([&](Voice& v) { v.render(out, pos, out.numSamples - pos); });
    if (!shared_.stereo) std::memcpy(out.right, out.left, sizeof(float) * static_cast<size_t>(out.numSamples));

    const FaustInstance* ro = &instances_.front();
    for (int i = 0; i < kMaxVoices; ++i)
        if (alloc_.voice(i).active && alloc_.voice(i).inst) {
            ro = alloc_.voice(i).inst;
            break;
        }
    pullReadOnly(*ro);
    active_.store(alloc_.activeCount(), std::memory_order_relaxed);
}

void FaustInstrument::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    if (!valid_ || faulted_ || instances_.empty()) return; // buffer is already cleared
    curOut_ = &out;
    curEvents_ = events;
    curCtx_ = &ctx;
    FpuGuard fpu;
    uint32_t code = 0;
    if (!platform::guardedCall(&FaustInstrument::guardedProcess, this, &code)) {
        fpu.restore();
        fault(code);
        out.clear();
        active_.store(0, std::memory_order_relaxed);
    }
}

// --- effect --------------------------------------------------------------------------------------------------

FaustEffect::FaustEffect(std::shared_ptr<const FaustPluginVersion> v) : FaustModuleBase(std::move(v)) { bindEnums(); }

void FaustEffect::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
    for (auto& i : instances_) faust::deleteInstance(i.dsp);
    instances_.clear();
    const bool dualMono = v_->numInputs == 1 && v_->numOutputs == 1;
    instances_.resize(dualMono ? 2 : 1);
    valid_ = true;
    for (auto& inst : instances_) valid_ = valid_ && makeInstance(inst);
    inL_.assign(static_cast<size_t>(maxBlock), 0.0f);
    inR_.assign(static_cast<size_t>(maxBlock), 0.0f);
    faulted_ = false;
}

void FaustEffect::reset() {
    for (auto& i : instances_) faust::clear(i.dsp);
}

int FaustEffect::tailSamples() const { return static_cast<int>(v_->tailSeconds * static_cast<float>(sampleRate_)); }

void FaustEffect::guardedProcess(void* self) { static_cast<FaustEffect*>(self)->processBody(); }

void FaustEffect::processBody() noexcept {
    AudioBlock& io = *cur_;
    const int n = io.numSamples;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    for (auto& inst : instances_) pushParams(inst);
    // Separate input buffers: Faust (esp. -vec code) must not see aliased in/out buffers.
    std::memcpy(inL_.data(), io.left, bytes);
    std::memcpy(inR_.data(), io.right, bytes);
    const int ni = v_->numInputs, no = v_->numOutputs;
    if (instances_.size() == 2) { // dual mono
        float* i0[1] = {inL_.data()};
        float* o0[1] = {io.left};
        float* i1[1] = {inR_.data()};
        float* o1[1] = {io.right};
        faust::compute(instances_[0].dsp, n, i0, o0);
        faust::compute(instances_[1].dsp, n, i1, o1);
    } else {
        if (ni == 1)
            for (int i = 0; i < n; ++i) inL_[static_cast<size_t>(i)] = 0.5f * (inL_[static_cast<size_t>(i)] + inR_[static_cast<size_t>(i)]);
        float* ins[2] = {inL_.data(), inR_.data()};
        float* outs[2] = {io.left, io.right};
        faust::compute(instances_[0].dsp, n, ins, outs);
        if (no == 1) std::memcpy(io.right, io.left, bytes);
    }
    if (!finiteBlock(io.left, n) || !finiteBlock(io.right, n)) {
        io.clear();
        for (auto& inst : instances_) faust::clear(inst.dsp);
        v_->reportFault(kFaultNonFinite);
    }
    pullReadOnly(instances_.front());
}

void FaustEffect::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    if (!valid_ || faulted_ || instances_.empty()) {
        if (faulted_) io.clear(); // muted
        return;                   // invalid: passthrough
    }
    cur_ = &io;
    FpuGuard fpu;
    uint32_t code = 0;
    if (!platform::guardedCall(&FaustEffect::guardedProcess, this, &code)) {
        fpu.restore();
        fault(code);
        io.clear();
    }
}

} // namespace ks::plugins
