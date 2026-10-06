#include "instruments/sampler/SamplerModule.h"

#if defined(KS_HAS_SFIZZ)

#include "core/AppPaths.h"
#include "core/RtCheck.h"
#include "dsp/Math.h"
#include "instruments/sampler/SamplePaths.h"

#include <sfizz.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace ks {

namespace {
std::atomic<int> gLoads{0};
std::atomic<int> gLiveCores{0};
std::atomic<int> gTestLoadDelayMs{0};
std::atomic<int> gTestPostLoadDelayMs{0};

// Process-wide loader bookkeeping. Loads are serialized (one sfizz_load_file at a time: bounded RAM / disk IO when
// presets are switched quickly) and a load whose module is already gone is skipped before it starts. Leaked on
// purpose: detached loader threads may still use it while static destructors run.
struct LoaderGate {
    std::mutex loadMutex;            // held by a loader thread for the duration of one load
    std::atomic<int> threads{0};     // loader threads alive (incl. detached ones)
};
LoaderGate& gate() {
    static LoaderGate* g = new LoaderGate();
    return *g;
}
// At exit, give detached loads a bounded chance to finish before the process tears down sfizz statics.
struct ExitWaiter {
    ~ExitWaiter() {
        for (int i = 0; i < 1000 && gate().threads.load() > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
} gExitWaiter;

const std::filesystem::path& repoRoot() {
    static const std::filesystem::path root = AppPaths::discover().root;
    return root;
}
} // namespace

// Shared between the module (control + audio thread) and its loader thread.
struct SamplerModule::Core {
    enum State : int { Idle = 0, Loading, Ready, Paused, Failed };

    sfizz_synth_t* synth = nullptr; // created/loaded by the loader thread; used by the audio thread when Ready

    // Access handshake (seq_cst): the audio thread sets `busy`, then reads `state`; the loader thread stores
    // `Paused`, then waits for `busy == false`. Either the audio thread sees Paused or the loader sees busy.
    std::atomic<int> state{Idle};
    std::atomic<bool> busy{false};

    std::atomic<bool> stop{false};
    std::atomic<bool> offline{false};
    std::atomic<int> regions{0};
    std::atomic<int> wantPolyphony{64}; // written by the audio thread (param), applied by the loader thread
    std::atomic<double> loadSeconds{0.0};

    double sampleRate = 48000.0;
    int maxBlock = 512;
    std::filesystem::path path;

    std::mutex m; // guards `error` and the cv (never touched by the audio thread)
    std::condition_variable cv;
    std::string error;

    Core() { gLiveCores.fetch_add(1); }
    ~Core() {
        if (synth) sfizz_free(synth);
        gLiveCores.fetch_sub(1);
    }
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    void setError(std::string e) {
        std::lock_guard<std::mutex> lk(m);
        error = std::move(e);
    }

    // Loader thread: exclusive access to the synth while the audio thread renders silence.
    template <typename F>
    void withAudioPaused(F&& f) {
        state.store(Paused);
        while (busy.load()) std::this_thread::yield();
        f();
        state.store(Ready);
    }

    // Loader thread entry. `gate().threads` was incremented by the spawner; released after the Core ref is dropped.
    static void run(std::shared_ptr<Core> c) {
        runImpl(c);
        c.reset();
        gate().threads.fetch_sub(1);
    }

    static void runImpl(const std::shared_ptr<Core>& c) {
        for (int waited = 0; waited < gTestLoadDelayMs.load() && !c->stop.load(); waited += 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto t0 = std::chrono::steady_clock::now();
        sfizz_synth_t* s = sfizz_create_synth();
        sfizz_set_sample_rate(s, static_cast<float>(c->sampleRate));
        sfizz_set_samples_per_block(s, c->maxBlock);
        sfizz_set_preload_size(s, static_cast<unsigned>(kPreloadFrames));
        int polyphony = std::clamp(c->wantPolyphony.load(), 1, kMaxPolyphony);
        sfizz_set_num_voices(s, polyphony);
        // Offline renders (freewheeling) use the live interpolation quality so they match what is heard live.
        sfizz_set_sample_quality(s, SFIZZ_PROCESS_FREEWHEELING, sfizz_get_sample_quality(s, SFIZZ_PROCESS_LIVE));
        sfizz_set_oscillator_quality(s, SFIZZ_PROCESS_FREEWHEELING,
                                     sfizz_get_oscillator_quality(s, SFIZZ_PROCESS_LIVE));
        c->synth = s;

        const std::string utf8 = pathToUtf8(c->path);
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(gate().loadMutex);
            if (c->stop.load()) return; // module destroyed while queued: skip the IO
            ok = sfizz_load_file(s, utf8.c_str());
            gLoads.fetch_add(1);
        }
        const int regions = ok ? sfizz_get_num_regions(s) : 0;
        if (const int d = gTestPostLoadDelayMs.load(); d > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d));
        c->loadSeconds.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        c->regions.store(regions);
        if (c->stop.load()) return;
        if (!ok || regions <= 0) {
            c->setError(ok ? "no regions in '" + utf8 + "'" : "sfizz failed to load '" + utf8 + "'");
            c->state.store(Failed);
            return;
        }
        c->state.store(Ready);

        // Stay alive for non-RT reconfiguration (voice reallocation) until the module goes away.
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(c->m);
                c->cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return c->stop.load(); });
            }
            if (c->stop.load()) return;
            const int want = std::clamp(c->wantPolyphony.load(), 1, kMaxPolyphony);
            if (want != polyphony) {
                c->withAudioPaused([&] { sfizz_set_num_voices(s, want); });
                polyphony = want;
            }
        }
    }
};

namespace {
// Changing polyphony reallocates sfizz voices (loader thread): sounding notes are cut. Not for automation.
ParamSpec polyphonyParam() {
    ParamSpec p = intParam("polyphony", "Polyphony", 1, SamplerModule::kMaxPolyphony, 64, {}, "Voice");
    p.flags = ParamFlags::NonAutomatable;
    return p;
}
} // namespace

const ModuleInfo& SamplerModule::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "sampler";
        i.displayName = "SFZ Sampler";
        i.kind = ModuleKind::Instrument;
        i.category = "Sampler";
        ParamSpec loading = boolParam("loading", "Loading", false, "Status");
        loading.flags = ParamFlags::ReadOnly | ParamFlags::NonAutomatable;
        ParamSpec regions = intParam("loaded_regions", "Regions", 0, 100000, 0, {}, "Status");
        regions.flags = ParamFlags::ReadOnly | ParamFlags::NonAutomatable;
        i.params = {
            linearParam("volume_db", "Volume", -60.0f, 12.0f, 0.0f, "dB", "Output"),
            linearParam("pan", "Pan", -1.0f, 1.0f, 0.0f, {}, "Output"),
            intParam("transpose", "Transpose", -24, 24, 0, "st", "Pitch"),
            linearParam("tune", "Tune", -100.0f, 100.0f, 0.0f, "ct", "Pitch"),
            polyphonyParam(),
            // Exponent 2^curve applied to note-on velocity: < 0 softer (easier to play loud), > 0 harder.
            linearParam("velocity_curve", "Velocity Curve", -1.0f, 1.0f, 0.0f, {}, "Voice"),
            loading,
            regions,
        };
        i.uiHints = {{"groupOrder", {"Output", "Pitch", "Voice", "Status"}},
                     {"front", {"volume_db", "pan", "transpose", "velocity_curve"}}};
        return i;
    }();
    return info;
}

SamplerModule::SamplerModule() : Module(moduleInfo()) {}

SamplerModule::~SamplerModule() { stopCore(); }

int SamplerModule::totalLoads() noexcept { return gLoads.load(); }
void SamplerModule::setTestLoadDelayMs(int ms) noexcept { gTestLoadDelayMs.store(ms); }
void SamplerModule::setTestPostLoadDelayMs(int ms) noexcept { gTestPostLoadDelayMs.store(ms); }
int SamplerModule::liveCores() noexcept { return gLiveCores.load(); }

void SamplerModule::loadState(const nlohmann::json& state) {
    sfz_.clear();
    if (state.is_object()) {
        if (auto it = state.find("sfz"); it != state.end() && it->is_string()) sfz_ = it->get<std::string>();
    }
}

nlohmann::json SamplerModule::saveState() const {
    if (sfz_.empty()) return nlohmann::json::object();
    return {{"sfz", sfz_}};
}

void SamplerModule::stopCore() noexcept {
    if (!core_) return;
    core_->stop.store(true);
    {
        std::lock_guard<std::mutex> lk(core_->m);
    }
    core_->cv.notify_all();
    if (loader_.joinable()) {
        // A running sfizz_load_file cannot be cancelled: let it finish on its own thread (it owns a Core ref).
        if (core_->state.load() == Core::Loading) loader_.detach();
        else loader_.join();
    }
    core_.reset();
    coreRaw_ = nullptr;
}

void SamplerModule::prepare(double sampleRate, int maxBlock) {
    stopCore();
    sampleRate_ = sampleRate;
    maxBlock_ = std::max(1, maxBlock);
    appliedOffline_ = false;
    synced_ = false;
    appliedTune_ = 0.0f;
    sent_ = SentControllers{};
    std::memset(sentNote_, 0, sizeof(sentNote_));
    activeVoices_.store(0);

    auto c = std::make_shared<Core>();
    c->sampleRate = sampleRate;
    c->maxBlock = maxBlock_;
    c->offline.store(offline_);
    c->wantPolyphony.store(static_cast<int>(params().get(Polyphony)));
    if (!sfz_.empty()) {
        SamplePathResult r = resolveSamplePath(sfz_, repoRoot());
        if (r.path) {
            c->path = *r.path;
            c->state.store(Core::Loading);
        } else {
            c->error = r.error;
            c->state.store(Core::Failed);
        }
    }
    {
        const float g = dsp::dbToGain(params().get(VolumeDb));
        const float pan = std::clamp(params().get(Pan), -1.0f, 1.0f);
        gainL_ = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
        gainR_ = g * (pan < 0.0f ? 1.0f + pan : 1.0f);
    }
    core_ = c;
    coreRaw_ = c.get();
    params().setRaw(Loading, c->state.load() == Core::Loading ? 1.0f : 0.0f);
    params().setRaw(LoadedRegions, 0.0f);
    if (c->state.load() == Core::Loading) {
        gate().threads.fetch_add(1);
        loader_ = std::thread(&Core::run, c);
    }
}

void SamplerModule::reset() {
    std::memset(sentNote_, 0, sizeof(sentNote_));
    activeVoices_.store(0, std::memory_order_relaxed);
    Core* c = coreRaw_;
    if (!c) return;
    c->busy.store(true);
    if (c->state.load() == Core::Ready) sfizz_all_sound_off(c->synth);
    c->busy.store(false);
}

bool SamplerModule::isReady() const {
    const Core* c = coreRaw_;
    return c == nullptr || c->state.load() != Core::Loading;
}

void SamplerModule::setOfflineMode(bool offline) {
    offline_ = offline;
    if (coreRaw_) coreRaw_->offline.store(offline);
}

int SamplerModule::regionCount() const noexcept {
    const Core* c = coreRaw_;
    return c ? c->regions.load() : 0;
}

bool SamplerModule::loadFailed() const noexcept {
    const Core* c = coreRaw_;
    return c != nullptr && c->state.load() == Core::Failed;
}

double SamplerModule::loadSeconds() const noexcept {
    const Core* c = coreRaw_;
    return c ? c->loadSeconds.load() : 0.0;
}

std::string SamplerModule::lastError() const {
    if (!core_) return {};
    std::lock_guard<std::mutex> lk(core_->m);
    return core_->error;
}

std::string SamplerModule::resolvedPath() const { return core_ ? pathToUtf8(core_->path) : std::string(); }

int SamplerModule::tailSamples() const { return static_cast<int>(sampleRate_ * 2.0); }

void SamplerModule::sendCc(sfizz_synth_t* s, int delay, int cc, float value) noexcept {
    sfizz_send_hdcc(s, delay, cc, value);
    switch (cc) {
    case 1: sent_.mod = value; break;
    case 11: sent_.expression = value; break;
    case 64: sent_.sustain = value; break;
    case 66: sent_.sostenuto = value; break;
    default: break;
    }
}

void SamplerModule::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    const int n = out.numSamples;
    Core* c = coreRaw_;
    ParamSet& p = params();
    if (c == nullptr || n <= 0) return;
    c->wantPolyphony.store(static_cast<int>(p.get(Polyphony)), std::memory_order_relaxed);

    c->busy.store(true);
    const int st = c->state.load();
    p.setRaw(Loading, st == Core::Loading ? 1.0f : 0.0f);
    p.setRaw(LoadedRegions, static_cast<float>(c->regions.load(std::memory_order_relaxed)));
    if (st != Core::Ready) {
        c->busy.store(false);
        // No access to the synth: events are dropped; held notes are forgotten (voices are reset on resume).
        synced_ = false;
        std::memset(sentNote_, 0, sizeof(sentNote_));
        activeVoices_.store(0, std::memory_order_relaxed);
        return; // buffer is cleared on entry
    }
    sfizz_synth_t* s = c->synth;

    const bool offline = c->offline.load(std::memory_order_relaxed);
    if (offline != appliedOffline_) {
        if (offline) sfizz_enable_freewheeling(s);
        else sfizz_disable_freewheeling(s);
        appliedOffline_ = offline;
    }
    const float tune = p.get(Tune);
    if (!synced_ || tune != appliedTune_) {
        sfizz_set_tuning_frequency(s, 440.0f * std::exp2(tune / 1200.0f));
        appliedTune_ = tune;
    }
    if (!synced_) {
        // (Re)gained access: re-send controller state that changed while events were dropped (loading/paused).
        // Compared with what sfizz last received; CC1/CC11 count as "sfz default" until first sent, so an
        // instrument's own `set_ccN` survives (MelloSFZotron maps pitch to CC1 with set_cc1=64).
        if (const ChannelState* cs = ctx.channel) {
            if (cs->sustain != (sent_.sustain >= 0.5f)) sendCc(s, 0, 64, cs->sustain ? 1.0f : 0.0f);
            if (cs->sostenuto != (sent_.sostenuto >= 0.5f)) sendCc(s, 0, 66, cs->sostenuto ? 1.0f : 0.0f);
            if (sent_.mod < 0.0f ? cs->modWheel > 0.0f : cs->modWheel != sent_.mod) sendCc(s, 0, 1, cs->modWheel);
            if (sent_.expression < 0.0f ? cs->expression < 1.0f : cs->expression != sent_.expression)
                sendCc(s, 0, 11, cs->expression);
            if (cs->pitchBend != sent_.bend) {
                sfizz_send_hd_pitch_wheel(s, 0, cs->pitchBend);
                sent_.bend = cs->pitchBend;
            }
        }
        synced_ = true;
    }

    const int transpose = static_cast<int>(p.get(Transpose));
    const float velExp = std::exp2(p.get(VelocityCurve));
    for (const MidiEvent& e : events) {
        const int delay = std::clamp(static_cast<int>(e.sampleOffset), 0, n - 1);
        const int key = e.data1 & 0x7f;
        switch (e.type) {
        case MidiEventType::NoteOn: {
            const int note = key + transpose;
            if (note < 0 || note > 127) break;
            if (sentNote_[key] != 0 && sentNote_[key] - 1 != note)
                sfizz_send_hd_note_off(s, delay, sentNote_[key] - 1, 0.0f);
            const float v = std::clamp(e.valueF > 0.0f ? e.valueF : static_cast<float>(e.value7) / 127.0f,
                                       1.0f / 127.0f, 1.0f);
            sfizz_send_hd_note_on(s, delay, note, std::max(1.0f / 127.0f, std::pow(v, velExp)));
            sentNote_[key] = static_cast<uint8_t>(note + 1);
            break;
        }
        case MidiEventType::NoteOff:
            if (sentNote_[key] != 0) {
                sfizz_send_hd_note_off(s, delay, sentNote_[key] - 1, e.valueF);
                sentNote_[key] = 0;
            }
            break;
        case MidiEventType::ControlChange: sendCc(s, delay, key, e.valueF); break;
        case MidiEventType::PitchBend:
            sent_.bend = std::clamp(e.valueF, -1.0f, 1.0f);
            sfizz_send_hd_pitch_wheel(s, delay, sent_.bend);
            break;
        case MidiEventType::ChannelPressure: sfizz_send_hd_channel_aftertouch(s, delay, e.valueF); break;
        case MidiEventType::PolyPressure:
            if (sentNote_[key] != 0) sfizz_send_hd_poly_aftertouch(s, delay, sentNote_[key] - 1, e.valueF);
            break;
        case MidiEventType::AllNotesOff:
            // Release everything regardless of the pedals (the next pedal CC re-syncs them).
            sendCc(s, delay, 64, 0.0f);
            sendCc(s, delay, 66, 0.0f);
            for (int k = 0; k < 128; ++k) {
                if (sentNote_[k] != 0) sfizz_send_hd_note_off(s, delay, sentNote_[k] - 1, 0.0f);
                sentNote_[k] = 0;
            }
            break;
        case MidiEventType::AllSoundOff:
            sfizz_all_sound_off(s);
            std::memset(sentNote_, 0, sizeof(sentNote_));
            break;
        default: break;
        }
    }

    float* channels[2] = {out.left, out.right};
    if (appliedOffline_) {
        // Freewheeling (offline only, never on a live device): sfizz waits for pending disk loads and frees the
        // finished loading jobs (std::future) inside render_block. Not real time by design, so not a violation.
        rt::RtAllowScope offlineIo;
        sfizz_render_block(s, channels, 2, n);
    } else {
        sfizz_render_block(s, channels, 2, n);
    }
    activeVoices_.store(sfizz_get_num_active_voices(s), std::memory_order_relaxed);
    c->busy.store(false);

    // Output gain + balance, linearly ramped across the block (no zipper on volume/pan moves).
    const float g = dsp::dbToGain(p.get(VolumeDb));
    const float pan = std::clamp(p.get(Pan), -1.0f, 1.0f);
    const float tl = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
    const float tr = g * (pan < 0.0f ? 1.0f + pan : 1.0f);
    const float inv = 1.0f / static_cast<float>(n);
    const float dl = (tl - gainL_) * inv, dr = (tr - gainR_) * inv;
    for (int i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i + 1);
        out.left[i] *= gainL_ + dl * fi;
        out.right[i] *= gainR_ + dr * fi;
    }
    gainL_ = tl;
    gainR_ = tr;
}

} // namespace ks

#endif // KS_HAS_SFIZZ
