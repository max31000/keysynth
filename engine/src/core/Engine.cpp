#include "core/Engine.h"

#include "core/RtCheck.h"
#include "dsp/Denormals.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ks {

Engine::Engine() {
    for (auto& s : sources_) s = std::make_unique<SpscQueue<MidiEvent>>(1024);
    events_.reserve(kMaxEventsPerBlock); // same limit as each layer's buffer
    prepare(48000.0, 512);
}

Engine::~Engine() = default;

void Engine::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock_ = std::clamp(maxBlock, 1, kMaxDeviceBlock);
    swapper_.prepare(sampleRate_, maxBlock_);
    scratchL_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    scratchR_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    limiter_.setCeilingDb(-0.3f);
    limiter_.setRelease(60.0f);
    limiter_.prepare(sampleRate_);
    metronome_.prepare(sampleRate_);
    sequencer_.stop();
    transport_.resetPlayState(); // still playing -> restarts (position 0, sequencer) on the next block
    countInEndPpq_ = -1.0;
    channels_ = {};
}

void Engine::setMidiSourceActive(int slot, bool active) noexcept {
    if (slot >= 0 && slot < kMaxMidiSources) sourceActive_[static_cast<size_t>(slot)].store(active);
}

void Engine::gatherEvents(MidiEventSpan extra) noexcept {
    events_.clear();
    const size_t cap = events_.capacity();
    MidiEvent e;
    // Panic first.
    const uint32_t p = panicRequests_.load(std::memory_order_relaxed);
    if (p != panicSeen_) {
        panicSeen_ = p;
        panicBlock_ = true;
        events_.push_back(MidiEvent::allSoundOff());
        channels_ = {};
        limiter_.reset();
        metronome_.reset();
    }
    while (events_.size() < cap && inbox_.pop(e)) {
        e.sampleOffset = 0;
        events_.push_back(e);
    }
    for (int s = 0; s < kMaxMidiSources; ++s) {
        // Inactive slots are drained too (events pushed when a device closed, e.g. its AllNotesOff).
        auto& q = *sources_[static_cast<size_t>(s)];
        while (events_.size() < cap && q.pop(e)) {
            e.sampleOffset = 0;
            events_.push_back(e);
        }
    }
    for (const MidiEvent& x : extra) {
        if (events_.size() >= cap) break;
        events_.push_back(x);
    }
    for (const MidiEvent& x : events_) {
        if (x.channel >= 1 && x.channel <= 16) channels_[x.channel].apply(x);
        channels_[0].apply(x);
        if (x.type == MidiEventType::NoteOn || x.type == MidiEventType::NoteOff) {
            TelemetryEvent te;
            te.type = x.type == MidiEventType::NoteOn ? TelemetryEvent::Type::NoteOn : TelemetryEvent::Type::NoteOff;
            te.note = x.data1;
            te.velocity = x.value7;
            te.channel = x.channel;
            telemetry_.pushEvent(te);
        }
    }
}

void Engine::processBlock(const AudioBlock& out, MidiEventSpan extra) noexcept {
    rt::RtScope rtScope;
    dsp::ScopedNoDenormals noDenormals;
    const int n = std::min(out.numSamples, maxBlock_);
    AudioBlock o{out.left, out.right, n};

    gatherEvents(extra);

    bool started = false, stopped = false;
    const TransportInfo t = transport_.beginBlock(started, stopped);

    // Drum sequencer (sample-accurate note-ons for the RhythmNode).
    if (started) {
        const double barPpq = 4.0 * t.numerator / static_cast<double>(t.denominator > 0 ? t.denominator : 4);
        countInEndPpq_ = rhythm_.countIn.load(std::memory_order_relaxed) ? barPpq : -1.0;
        sequencer_.start(countInEndPpq_ > 0.0 ? countInEndPpq_ : 0.0);
    } else if (stopped) {
        sequencer_.stop();
        countInEndPpq_ = -1.0;
    }
    int nRhythm = 0;
    if (panicBlock_) rhythmEvents_[static_cast<size_t>(nRhythm++)] = MidiEvent::allSoundOff();
    panicBlock_ = false;
    nRhythm += sequencer_.generate(t, n, sampleRate_, rhythm_.swing.load(std::memory_order_relaxed),
                                   rhythm_.drumsEnabled.load(std::memory_order_relaxed), rhythmEvents_.data() + nRhythm,
                                   kMaxRhythmEvents - nRhythm);

    RenderArgs args;
    args.ctx.sampleRate = sampleRate_;
    args.ctx.numSamples = n;
    args.ctx.sampleTime = sampleTime_;
    args.ctx.transport = t;
    args.channels = channels_.data();
    args.rhythmEvents = MidiEventSpan(rhythmEvents_.data(), static_cast<size_t>(nRhythm));
    args.rhythmGain = rhythm_.drumsVolume.load(std::memory_order_relaxed);
    swapper_.render(o, MidiEventSpan(events_.data(), events_.size()), args);

    metronome_.process(o, t, started, sampleRate_, countInEndPpq_);
    limiter_.process(o.left, o.right, n);

    // Telemetry
    float pl = 0.0f, pr = 0.0f;
    o.peak(pl, pr);
    Telemetry::raise(telemetry_.masterL, pl);
    Telemetry::raise(telemetry_.masterR, pr);
    if (const RackGraph* g = swapper_.current()) {
        const int nl = std::min(static_cast<int>(g->layers.size()), Telemetry::kMaxLayers);
        for (int i = 0; i < nl; ++i) {
            auto& m = telemetry_.layers[static_cast<size_t>(i)];
            const LayerNode& ln = *g->layers[static_cast<size_t>(i)];
            if (m.node.load(std::memory_order_relaxed) != ln.node) {
                m.node.store(ln.node, std::memory_order_relaxed);
                m.l.store(0.0f, std::memory_order_relaxed);
                m.r.store(0.0f, std::memory_order_relaxed);
            }
            Telemetry::raise(m.l, ln.meterL.load(std::memory_order_relaxed));
            Telemetry::raise(m.r, ln.meterR.load(std::memory_order_relaxed));
        }
        telemetry_.layerCount.store(nl, std::memory_order_relaxed);
    } else {
        telemetry_.layerCount.store(0, std::memory_order_relaxed);
    }
    telemetry_.voices.store(swapper_.activeVoices(), std::memory_order_relaxed);
    telemetry_.limiterReductionDb.store(limiter_.gainReductionDb(), std::memory_order_relaxed);
    telemetry_.blocks.fetch_add(1, std::memory_order_relaxed);

    transport_.endBlock(n, sampleRate_);
    telemetry_.ppq.store(transport_.ppqApprox(), std::memory_order_relaxed);
    sampleTime_ += n;
    sampleTimeShared_.store(sampleTime_, std::memory_order_relaxed);
}

void Engine::process(float* const* outputs, int numOutputs, int numSamples) noexcept {
    rt::RtScope rtScope;
    const auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    while (done < numSamples) {
        const int n = std::min(maxBlock_, numSamples - done);
        AudioBlock b{scratchL_.data(), scratchR_.data(), n};
        processBlock(b);
        if (numOutputs >= 2) {
            if (outputs[0]) std::memcpy(outputs[0] + done, b.left, sizeof(float) * static_cast<size_t>(n));
            if (outputs[1]) std::memcpy(outputs[1] + done, b.right, sizeof(float) * static_cast<size_t>(n));
        } else if (numOutputs == 1 && outputs[0]) {
            for (int i = 0; i < n; ++i) outputs[0][done + i] = 0.5f * (b.left[i] + b.right[i]);
        }
        done += n;
    }
    for (int c = 2; c < numOutputs; ++c)
        if (outputs[c]) std::memset(outputs[c], 0, sizeof(float) * static_cast<size_t>(numSamples));

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    const double budget = static_cast<double>(numSamples) / sampleRate_;
    const float load = budget > 0 ? static_cast<float>(elapsed / budget) : 0.0f;
    Telemetry::raise(telemetry_.cpuLoad, load);
    if (load > 1.0f) telemetry_.overloads.fetch_add(1, std::memory_order_relaxed);
}

} // namespace ks
