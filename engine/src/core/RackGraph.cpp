#include "core/RackGraph.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cstring>

namespace ks {

// --- RenderCache -------------------------------------------------------------------------------------------

void RenderCache::prepare(int maxBlock) {
    maxBlock_ = maxBlock;
    storage_.assign(static_cast<size_t>(kSlots) * 2 * static_cast<size_t>(maxBlock), 0.0f);
    clear();
}

void RenderCache::clear() noexcept {
    for (auto& e : entries_) e = Entry{};
    used_ = 0;
}

int RenderCache::slotFor(const Module* m) noexcept {
    for (int i = 0; i < used_; ++i)
        if (entries_[static_cast<size_t>(i)].module == m) return i;
    if (used_ >= kSlots) return -1;
    entries_[static_cast<size_t>(used_)] = Entry{m, 0};
    return used_++;
}

void RenderCache::store(int slot, const AudioBlock& b) noexcept {
    if (slot < 0 || slot >= used_ || b.numSamples > maxBlock_) return;
    float* base = storage_.data() + static_cast<size_t>(slot) * 2 * static_cast<size_t>(maxBlock_);
    std::memcpy(base, b.left, sizeof(float) * static_cast<size_t>(b.numSamples));
    std::memcpy(base + maxBlock_, b.right, sizeof(float) * static_cast<size_t>(b.numSamples));
    entries_[static_cast<size_t>(slot)].stamp = stamp_;
}

bool RenderCache::load(int slot, const AudioBlock& b) const noexcept {
    if (slot < 0 || slot >= used_ || b.numSamples > maxBlock_) return false;
    if (entries_[static_cast<size_t>(slot)].stamp != stamp_) return false;
    const float* base = storage_.data() + static_cast<size_t>(slot) * 2 * static_cast<size_t>(maxBlock_);
    std::memcpy(b.left, base, sizeof(float) * static_cast<size_t>(b.numSamples));
    std::memcpy(b.right, base + maxBlock_, sizeof(float) * static_cast<size_t>(b.numSamples));
    return true;
}

// --- LayerNode ---------------------------------------------------------------------------------------------

void LayerNode::setZone(const Zone& z) noexcept {
    keyLo.store(z.keyLo);
    keyHi.store(z.keyHi);
    velLo.store(z.velLo);
    velHi.store(z.velHi);
    transpose.store(z.transpose);
    channel.store(z.channel);
    volumeDb.store(z.volumeDb);
    pan.store(z.pan);
    mute.store(z.mute);
    solo.store(z.solo);
    sustain.store(z.sustain);
}

Zone LayerNode::zone() const noexcept {
    Zone z;
    z.keyLo = keyLo.load();
    z.keyHi = keyHi.load();
    z.velLo = velLo.load();
    z.velHi = velHi.load();
    z.transpose = transpose.load();
    z.channel = channel.load();
    z.volumeDb = volumeDb.load();
    z.pan = pan.load();
    z.mute = mute.load();
    z.solo = solo.load();
    z.sustain = sustain.load();
    return z;
}

void LayerNode::filterEvents(MidiEventSpan in, std::vector<MidiEvent>& out) noexcept {
    out.clear();
    const int lo = keyLo.load(std::memory_order_relaxed), hi = keyHi.load(std::memory_order_relaxed);
    const int vlo = velLo.load(std::memory_order_relaxed), vhi = velHi.load(std::memory_order_relaxed);
    const int tr = transpose.load(std::memory_order_relaxed);
    const int ch = channel.load(std::memory_order_relaxed);
    const bool sus = sustain.load(std::memory_order_relaxed);
    for (const MidiEvent& e : in) {
        if (out.size() >= static_cast<size_t>(kMaxEventsPerBlock)) break;
        const bool chanOk = ch == 0 || e.channel == 0 || e.channel == ch;
        // Note-offs / poly pressure for sounding notes pass even if the zone channel changed meanwhile.
        if (!chanOk && e.type != MidiEventType::NoteOff && e.type != MidiEventType::PolyPressure) continue;
        const size_t mapIdx = static_cast<size_t>(((e.channel == 0 ? 1 : e.channel) - 1) * 128 + e.data1);
        switch (e.type) {
        case MidiEventType::NoteOn: {
            const int key = e.data1;
            if (key < lo || key > hi || e.value7 < vlo || e.value7 > vhi) break;
            const int note = key + tr;
            if (note < 0 || note > 127) break;
            noteMap[mapIdx].store(static_cast<uint8_t>(note + 1), std::memory_order_relaxed);
            MidiEvent t = e;
            t.data1 = static_cast<uint8_t>(note);
            out.push_back(t);
            break;
        }
        case MidiEventType::NoteOff: {
            const uint8_t mapped = noteMap[mapIdx].exchange(0, std::memory_order_relaxed);
            int note = -1;
            if (mapped != 0) note = mapped - 1;
            else if (chanOk && e.data1 >= lo && e.data1 <= hi) note = e.data1 + tr; // unmapped (graph swap): best effort
            if (note < 0 || note > 127) break;
            MidiEvent t = e;
            t.data1 = static_cast<uint8_t>(note);
            out.push_back(t);
            break;
        }
        case MidiEventType::PolyPressure: {
            const uint8_t mapped = noteMap[mapIdx].load(std::memory_order_relaxed);
            if (mapped == 0) break;
            MidiEvent t = e;
            t.data1 = static_cast<uint8_t>(mapped - 1);
            out.push_back(t);
            break;
        }
        case MidiEventType::ControlChange:
            if ((e.data1 == 64 || e.data1 == 66) && !sus) break;
            out.push_back(e);
            break;
        case MidiEventType::AllNotesOff:
        case MidiEventType::AllSoundOff:
            for (auto& m : noteMap) m.store(0, std::memory_order_relaxed);
            out.push_back(e);
            break;
        default: out.push_back(e); break;
        }
    }
}

// --- RackGraph ---------------------------------------------------------------------------------------------

RackGraph::RackGraph(double sampleRate, int maxBlock)
    : sampleRate_(sampleRate), maxBlock_(maxBlock) {
    masterGain_.prepare(sampleRate, 0.01f);
}

RackGraph::~RackGraph() = default;

void RackGraph::finalize() {
    moduleIndex_.clear();
    layerIndex_.clear();
    masterGain_.snap(dsp::dbToGain(masterVolumeDb.load()));
    for (auto& l : layers) {
        layerIndex_[l->node] = l.get();
        moduleIndex_[l->instrument.node] = &l->instrument;
        for (auto& f : l->fx) moduleIndex_[f->node] = f.get();
        l->events.reserve(kMaxEventsPerBlock);
        l->bufL.assign(static_cast<size_t>(maxBlock_), 0.0f);
        l->bufR.assign(static_cast<size_t>(maxBlock_), 0.0f);
        l->gainL.prepare(sampleRate_, 0.01f);
        l->gainR.prepare(sampleRate_, 0.01f);
        const Zone z = l->zone();
        const float g = z.mute ? 0.0f : dsp::dbToGain(z.volumeDb);
        l->gainL.snap(g * (z.pan <= 0.0f ? 1.0f : 1.0f - z.pan));
        l->gainR.snap(g * (z.pan >= 0.0f ? 1.0f : 1.0f + z.pan));
    }
    for (auto& f : masterFx) moduleIndex_[f->node] = f.get();
    if (rhythm.drums.node != 0) moduleIndex_[rhythm.drums.node] = &rhythm.drums;
    rhythm.bufL.assign(static_cast<size_t>(maxBlock_), 0.0f);
    rhythm.bufR.assign(static_cast<size_t>(maxBlock_), 0.0f);
}

ModuleNode* RackGraph::findModuleNode(NodeId node) const {
    auto it = moduleIndex_.find(node);
    return it == moduleIndex_.end() ? nullptr : it->second;
}

Module* RackGraph::findModule(NodeId node) const {
    auto* n = findModuleNode(node);
    return n ? n->module.get() : nullptr;
}

LayerNode* RackGraph::findLayer(NodeId node) const {
    auto it = layerIndex_.find(node);
    return it == layerIndex_.end() ? nullptr : it->second;
}

std::vector<ModuleNode*> RackGraph::allModuleNodes() const {
    std::vector<ModuleNode*> out;
    for (auto& l : layers) {
        out.push_back(&l->instrument);
        for (auto& f : l->fx) out.push_back(f.get());
    }
    for (auto& f : masterFx) out.push_back(f.get());
    out.push_back(const_cast<ModuleNode*>(&rhythm.drums));
    return out;
}

int RackGraph::maxTailSamples() const noexcept {
    int t = 0;
    for (auto& l : layers) {
        if (l->instrument.module) t = std::max(t, l->instrument.module->tailSamples());
        for (auto& f : l->fx)
            if (f->module) t = std::max(t, f->module->tailSamples());
    }
    for (auto& f : masterFx)
        if (f->module) t = std::max(t, f->module->tailSamples());
    if (rhythm.drums.module) t = std::max(t, rhythm.drums.module->tailSamples());
    return t;
}

int RackGraph::activeVoices() const noexcept {
    int v = 0;
    for (auto& l : layers)
        if (l->instrument.module) v += l->instrument.module->activeVoices();
    if (rhythm.drums.module) v += rhythm.drums.module->activeVoices();
    return v;
}

void RackGraph::clearCacheSlots() noexcept {
    for (auto& l : layers) {
        l->instrument.cacheSlot = -1;
        for (auto& f : l->fx) f->cacheSlot = -1;
    }
    for (auto& f : masterFx) f->cacheSlot = -1;
    rhythm.drums.cacheSlot = -1;
}

void RackGraph::processNode(ModuleNode& n, AudioBlock& block, MidiEventSpan events, const ProcessContext& ctx,
                            const RenderArgs& args) noexcept {
    if (!n.module) return;
    if (args.readCache && n.cacheSlot != -1) {
        // Shared module in the old graph: never process it a second time (state would advance twice).
        if (n.cacheSlot >= 0 && args.cache && args.cache->load(n.cacheSlot, block)) return;
        if (n.module->info().kind == ModuleKind::Instrument) block.clear(); // effects: pass through
        return;
    }
    n.module->process(block, events, ctx);
    if (n.cacheSlot >= 0 && args.writeCache && args.cache) args.cache->store(n.cacheSlot, block);
}

void RackGraph::render(const AudioBlock& out, MidiEventSpan events, const RenderArgs& args) noexcept {
    const int n = std::min(out.numSamples, maxBlock_);
    out.clear();
    bool anySolo = false;
    for (auto& l : layers) anySolo = anySolo || l->solo.load(std::memory_order_relaxed);

    static constexpr ChannelState kDefaultState{};
    for (auto& lp : layers) {
        LayerNode& l = *lp;
        l.filterEvents(events, l.events);
        AudioBlock lb{l.bufL.data(), l.bufR.data(), n};
        lb.clear();

        const int ch = l.channel.load(std::memory_order_relaxed);
        ChannelState cs = args.channels ? args.channels[ch] : kDefaultState;
        if (!l.sustain.load(std::memory_order_relaxed)) cs.sustain = cs.sostenuto = false;
        ProcessContext ctx = args.ctx;
        ctx.numSamples = n;
        ctx.channel = &cs;

        if (l.instrument.module && l.instrument.module->info().kind == ModuleKind::Instrument)
            processNode(l.instrument, lb, MidiEventSpan(l.events.data(), l.events.size()), ctx, args);
        for (auto& f : l.fx) {
            if (f->bypass.load(std::memory_order_relaxed)) continue;
            if (f->module && f->module->info().kind != ModuleKind::Effect) continue;
            processNode(*f, lb, MidiEventSpan(l.events.data(), l.events.size()), ctx, args);
        }

        const bool silent = l.mute.load(std::memory_order_relaxed) ||
                            (anySolo && !l.solo.load(std::memory_order_relaxed));
        const float g = silent ? 0.0f : dsp::dbToGain(l.volumeDb.load(std::memory_order_relaxed));
        const float pan = l.pan.load(std::memory_order_relaxed);
        l.gainL.setTarget(g * (pan <= 0.0f ? 1.0f : 1.0f - pan));
        l.gainR.setTarget(g * (pan >= 0.0f ? 1.0f : 1.0f + pan));
        float pl = 0.0f, pr = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float a = lb.left[i] * l.gainL.next();
            const float b = lb.right[i] * l.gainR.next();
            out.left[i] += a;
            out.right[i] += b;
            pl = std::max(pl, std::fabs(a));
            pr = std::max(pr, std::fabs(b));
        }
        l.meterL.store(pl, std::memory_order_relaxed);
        l.meterR.store(pr, std::memory_order_relaxed);
    }

    ChannelState omni = args.channels ? args.channels[0] : kDefaultState;
    ProcessContext mctx = args.ctx;
    mctx.numSamples = n;
    mctx.channel = &omni;

    // Rhythm node (drum sequencer), before master FX.
    if (rhythm.drums.module && rhythm.drums.module->info().kind == ModuleKind::Instrument &&
        static_cast<int>(rhythm.bufL.size()) >= n) {
        AudioBlock rb{rhythm.bufL.data(), rhythm.bufR.data(), n};
        rb.clear();
        processNode(rhythm.drums, rb, args.rhythmEvents, mctx, args);
        const float g1 = std::clamp(args.rhythmGain, 0.0f, 2.0f);
        const float g0 = rhythm.lastGain < 0.0f ? g1 : rhythm.lastGain;
        const float step = n > 0 ? (g1 - g0) / static_cast<float>(n) : 0.0f;
        for (int i = 0; i < n; ++i) {
            const float g = g0 + step * static_cast<float>(i + 1);
            out.left[i] += rb.left[i] * g;
            out.right[i] += rb.right[i] * g;
        }
        rhythm.lastGain = g1;
    }

    AudioBlock mb{out.left, out.right, n};
    for (auto& f : masterFx) {
        if (f->bypass.load(std::memory_order_relaxed)) continue;
        if (f->module && f->module->info().kind != ModuleKind::Effect) continue;
        processNode(*f, mb, MidiEventSpan(), mctx, args);
    }
    masterGain_.setTarget(dsp::dbToGain(masterVolumeDb.load(std::memory_order_relaxed)));
    for (int i = 0; i < n; ++i) {
        const float g = masterGain_.next();
        out.left[i] *= g;
        out.right[i] *= g;
    }
}

} // namespace ks
