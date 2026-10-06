#include "core/GraphSwapper.h"

#include "core/RtCheck.h"

#include <algorithm>
#include <cmath>

namespace ks {

GraphSwapper::GraphSwapper() { prepare(48000.0, 512); }

GraphSwapper::~GraphSwapper() {
    RackGraph* g = nullptr;
    while (retire_.pop(g)) {}
    pending_.store(nullptr);
    owned_.clear();
}

void GraphSwapper::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlock;
    RackGraph* g = nullptr;
    while (retire_.pop(g)) {}
    pending_.store(nullptr);
    current_ = old_ = nullptr;
    backlogSize_ = 0;
    latest_ = nullptr;
    owned_.clear();
    cache_.prepare(maxBlock);
    oldL_.assign(static_cast<size_t>(maxBlock), 0.0f);
    oldR_.assign(static_cast<size_t>(maxBlock), 0.0f);
    xfadeLen_ = std::max<int64_t>(1, static_cast<int64_t>(kCrossfadeSeconds * sampleRate));
    fadeLen_ = std::max<int64_t>(1, static_cast<int64_t>(kFadeSeconds * sampleRate));
}

void GraphSwapper::publish(std::unique_ptr<RackGraph> graph) {
    if (!graph) return;
    RackGraph* raw = graph.get();
    owned_.push_back(std::move(graph));
    latest_ = raw;
    if (RackGraph* never = pending_.exchange(raw, std::memory_order_acq_rel)) {
        // The audio thread never took it: delete here (control thread).
        auto it = std::find_if(owned_.begin(), owned_.end(), [&](auto& p) { return p.get() == never; });
        if (it != owned_.end()) owned_.erase(it);
    }
}

void GraphSwapper::collectGarbage() {
    RackGraph* g = nullptr;
    while (retire_.pop(g)) {
        auto it = std::find_if(owned_.begin(), owned_.end(), [&](auto& p) { return p.get() == g; });
        if (it != owned_.end()) owned_.erase(it);
    }
}

int GraphSwapper::activeVoices() const noexcept {
    int v = current_ ? current_->activeVoices() : 0;
    if (old_) v += old_->activeVoices();
    return v;
}

void GraphSwapper::begin(RackGraph* next) noexcept {
    transitions_.fetch_add(1, std::memory_order_relaxed);
    if (current_ == nullptr) {
        current_ = next;
        return;
    }
    old_ = current_;
    current_ = next;
    cache_.clear();
    old_->clearCacheSlots();
    current_->clearCacheSlots();

    // Find modules shared by both graphs (pointer equality). No allocation: nested loops over fixed structure.
    // A node's cached output is only valid for the old chain if everything upstream of it is shared as well;
    // otherwise the old graph passes through it (kSharedNoCache). Shared modules are never processed twice.
    bool shared = false;
    bool upstreamShared = true;
    auto matchIn = [&](ModuleNode& nn) {
        if (!nn.module) return;
        const Module* m = nn.module.get();
        auto tryNode = [&](ModuleNode& on) {
            if (on.module.get() != m) return false;
            int slot = upstreamShared ? cache_.slotFor(m) : -1;
            if (slot < 0) slot = ModuleNode::kSharedNoCache;
            nn.cacheSlot = slot;
            on.cacheSlot = slot;
            shared = true;
            return true;
        };
        for (auto& ol : old_->layers) {
            if (tryNode(ol->instrument)) return;
            for (auto& of : ol->fx)
                if (tryNode(*of)) return;
        }
        for (auto& of : old_->masterFx)
            if (tryNode(*of)) return;
    };
    auto track = [&](ModuleNode& nn) {
        matchIn(nn);
        if (nn.module == nullptr || nn.cacheSlot < 0) upstreamShared = false;
    };
    bool allShared = current_->layers.size() == old_->layers.size();
    for (auto& nl : current_->layers) {
        upstreamShared = true;
        track(nl->instrument);
        for (auto& nf : nl->fx) track(*nf);
        allShared = allShared && upstreamShared;
    }
    upstreamShared = allShared;
    for (auto& nf : current_->masterFx) track(*nf);

    mode_ = shared ? Mode::Crossfade : Mode::TailOut;
    sendAllNotesOff_ = true;
    fading_ = false;
    pos_ = 0;
    fadePos_ = 0;
    silentRun_ = 0;
    const int64_t maxTail = static_cast<int64_t>(kMaxTailSeconds * sampleRate_);
    const int64_t minTail = static_cast<int64_t>(0.05 * sampleRate_);
    limit_ = std::clamp<int64_t>(old_->maxTailSamples(), minTail, maxTail);
}

void GraphSwapper::forceFade() noexcept {
    if (mode_ == Mode::TailOut && !fading_) {
        fading_ = true;
        fadePos_ = 0;
    }
}

bool GraphSwapper::finish() noexcept {
    // Flush backlog first.
    while (backlogSize_ > 0 && retire_.push(backlog_[static_cast<size_t>(backlogSize_ - 1)])) --backlogSize_;
    if (!retire_.push(old_)) {
        if (backlogSize_ >= static_cast<int>(backlog_.size())) return false; // keep rendering, retry next block
        backlog_[static_cast<size_t>(backlogSize_++)] = old_;
    }
    old_->clearCacheSlots();
    current_->clearCacheSlots();
    cache_.clear();
    old_ = nullptr;
    return true;
}

void GraphSwapper::render(const AudioBlock& out, MidiEventSpan events, const RenderArgs& args) noexcept {
    cache_.beginBlock();
    if (backlogSize_ > 0 && retire_.push(backlog_[static_cast<size_t>(backlogSize_ - 1)])) --backlogSize_;

    if (old_ == nullptr) {
        if (RackGraph* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) begin(p);
    } else if (pending_.load(std::memory_order_relaxed) != nullptr) {
        forceFade();
    }

    if (current_ == nullptr) {
        out.clear();
        return;
    }
    if (old_ == nullptr) {
        current_->render(out, events, args);
        return;
    }

    const int n = std::min(out.numSamples, maxBlock_);
    RenderArgs na = args;
    na.cache = &cache_;
    na.writeCache = true;
    na.readCache = false;
    current_->render(out, events, na);

    RenderArgs oa = args;
    oa.cache = &cache_;
    oa.writeCache = false;
    oa.readCache = true;
    AudioBlock ob{oldL_.data(), oldR_.data(), n};
    const MidiEvent off = MidiEvent::allNotesOff();
    MidiEventSpan oldEvents = sendAllNotesOff_ ? MidiEventSpan(&off, 1) : MidiEventSpan();
    sendAllNotesOff_ = false;
    old_->render(ob, oldEvents, oa);

    bool done = false;
    if (mode_ == Mode::Crossfade) {
        for (int i = 0; i < n; ++i) {
            const float t = std::min(1.0f, static_cast<float>(pos_ + i) / static_cast<float>(xfadeLen_));
            out.left[i] = out.left[i] * t + ob.left[i] * (1.0f - t);
            out.right[i] = out.right[i] * t + ob.right[i] * (1.0f - t);
        }
        pos_ += n;
        done = pos_ >= xfadeLen_;
    } else {
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) {
            float g = 1.0f;
            if (fading_) g = std::max(0.0f, 1.0f - static_cast<float>(fadePos_ + i) / static_cast<float>(fadeLen_));
            const float l = ob.left[i] * g, r = ob.right[i] * g;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
            out.left[i] += l;
            out.right[i] += r;
        }
        pos_ += n;
        if (fading_) {
            fadePos_ += n;
            done = fadePos_ >= fadeLen_;
        } else {
            silentRun_ = peak < 1e-5f ? silentRun_ + n : 0;
            if (silentRun_ >= 2048) done = true;
            else if (pos_ >= limit_) forceFade();
        }
    }
    if (done) finish();
}

} // namespace ks
