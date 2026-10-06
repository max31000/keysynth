// `sampler` (sfizz) tests. Fixtures (tiny WAV + SFZ) are generated into <root>/userdata/.test-sampler-<pid>/ (an
// allow-listed root, gitignored); unit tests never depend on downloaded libraries. The hidden [.samples] test renders
// every factory sampler preset whose library is installed (docs/TESTING.md).

#include "core/AppPaths.h"
#include "core/GraphBuilder.h"
#include "core/ModuleRegistry.h"
#include "core/RtCheck.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#if defined(KS_HAS_SFIZZ)
#include "instruments/sampler/SamplePaths.h"
#include "instruments/sampler/SamplerModule.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <thread>

using namespace ks;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000.0;

void writeSineWav(const fs::path& p, double hz, double seconds, float amp, int sr = 48000) {
    const uint32_t frames = static_cast<uint32_t>(seconds * sr);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + frames * 2);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(static_cast<uint32_t>(sr));
    u32(static_cast<uint32_t>(sr) * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const double x = amp * std::sin(2.0 * 3.14159265358979 * hz * i / sr);
        u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(x * 32767.0))));
    }
}

struct Fixture {
    fs::path dir;
    Fixture() {
        dir = AppPaths::discover().root / "userdata" / (".test-sampler-" + std::to_string(_getpid()));
        fs::create_directories(dir);
        writeSineWav(dir / "s220.wav", 220.0, 2.0, 0.3f);
        writeSineWav(dir / "s440.wav", 440.0, 2.0, 0.3f);
        writeSineWav(dir / "s660.wav", 660.0, 2.0, 0.3f);
        writeSineWav(dir / "s1000.wav", 1000.0, 1.0, 0.3f);
        sfz("map.sfz", "<global> amp_veltrack=0 ampeg_release=0.01\n"
                       "<region> sample=s440.wav lokey=60 hikey=72 pitch_keycenter=69\n"
                       "<region> sample=s220.wav lokey=40 hikey=59 pitch_keycenter=57\n");
        sfz("vel.sfz", "<group> lokey=0 hikey=127 pitch_keycenter=69 amp_veltrack=0 ampeg_release=0.01\n"
                       "<region> sample=s440.wav lovel=1 hivel=63\n"
                       "<region> sample=s660.wav lovel=64 hivel=127\n");
        sfz("delay.sfz", "<region> sample=s440.wav lokey=0 hikey=127 pitch_keycenter=69 amp_veltrack=0 "
                         "delay_random=0.012 ampeg_release=0.01\n");
        sfz("rel.sfz", "<global> amp_veltrack=0\n"
                       "<region> sample=s440.wav key=60 pitch_keycenter=60 ampeg_release=0.005\n"
                       "<region> sample=s1000.wav key=60 pitch_keycenter=60 trigger=release ampeg_release=0.005\n");
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void sfz(const char* name, const char* text) const {
        std::ofstream f(dir / name, std::ios::trunc);
        f << text;
    }
    std::string path(const char* name) const { return pathToUtf8(dir / name); }
};

const Fixture& fixture() {
    static const Fixture f;
    return f;
}

Patch samplerPatch(const std::string& sfz, std::map<std::string, float> params = {}) {
    Patch p;
    Layer l;
    l.node = 1;
    l.instrument.node = 2;
    l.instrument.type = "sampler";
    l.instrument.state = {{"sfz", sfz}};
    l.instrument.params = std::move(params);
    p.layers.push_back(l);
    return p;
}

// Dominant frequency by rising zero crossings in [t0, t1).
double estimateHz(const RenderResult& r, double t0, double t1) {
    const size_t a = static_cast<size_t>(t0 * r.sampleRate), b = static_cast<size_t>(t1 * r.sampleRate);
    int crossings = 0;
    for (size_t i = a + 1; i < b && i < r.left.size(); ++i)
        if (r.left[i - 1] < 0.0f && r.left[i] >= 0.0f) ++crossings;
    return crossings / (t1 - t0);
}

double rmsDb(const RenderResult& r, double t0, double t1) {
    const size_t a = static_cast<size_t>(t0 * r.sampleRate), b = std::min(r.left.size(), static_cast<size_t>(t1 * r.sampleRate));
    double sq = 0.0;
    for (size_t i = a; i < b; ++i) sq += static_cast<double>(r.left[i]) * r.left[i];
    const double rms = b > a ? std::sqrt(sq / static_cast<double>(b - a)) : 0.0;
    return rms > 1e-12 ? 20.0 * std::log10(rms) : -240.0;
}

RenderResult renderNotes(const Patch& p, std::vector<TimedEvent> ev, double tail = 0.3) {
    RenderOptions opt;
    opt.sampleRate = kSr;
    opt.blockSize = 64;
    opt.tailSeconds = tail;
    return OfflineRenderer::render(p, std::move(ev), opt);
}

bool waitReady(const Module& m, int timeoutMs = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!m.isReady()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Test load-delay hooks are process-global: always reset, even when a REQUIRE fails.
struct LoadDelayGuard {
    LoadDelayGuard(int before, int after) {
        SamplerModule::setTestLoadDelayMs(before);
        SamplerModule::setTestPostLoadDelayMs(after);
    }
    ~LoadDelayGuard() {
        SamplerModule::setTestLoadDelayMs(0);
        SamplerModule::setTestPostLoadDelayMs(0);
    }
};

struct Block {
    std::vector<float> l, r;
    AudioBlock ab;
    explicit Block(int n) : l(static_cast<size_t>(n)), r(static_cast<size_t>(n)), ab{l.data(), r.data(), n} {}
    void clear() { ab.clear(); }
    float peak() const {
        float pl = 0, pr = 0;
        ab.peak(pl, pr);
        return std::max(pl, pr);
    }
};

} // namespace

TEST_CASE("sampler: region mapping by key and pitch_keycenter", "[sampler]") {
    const Patch p = samplerPatch(fixture().path("map.sfz"));
    // 69 -> 440 (keycenter), 72 -> 523.3, 57 -> 220 (other region), 30 -> no region.
    const auto r = renderNotes(p, OfflineRenderer::notesToEvents({{69, 0.0, 0.5, 100},
                                                                  {72, 0.6, 0.5, 100},
                                                                  {57, 1.2, 0.5, 100},
                                                                  {30, 1.8, 0.5, 100}}));
    CHECK(std::fabs(estimateHz(r, 0.1, 0.45) - 440.0) < 6.0);
    CHECK(std::fabs(estimateHz(r, 0.7, 1.05) - 523.25) < 6.0);
    CHECK(std::fabs(estimateHz(r, 1.3, 1.65) - 220.0) < 6.0);
    CHECK(rmsDb(r, 1.85, 2.25) < -100.0);
    CHECK(rmsDb(r, 0.1, 0.45) > -30.0); // sfizz default global volume -7.35 dB, mono pan law
}

TEST_CASE("sampler: velocity layers and velocity curve", "[sampler]") {
    const std::string sfz = fixture().path("vel.sfz");
    {
        const auto r = renderNotes(samplerPatch(sfz), OfflineRenderer::notesToEvents({{69, 0.0, 0.4, 40}, {69, 0.5, 0.4, 100}}));
        CHECK(std::fabs(estimateHz(r, 0.05, 0.35) - 440.0) < 6.0);
        CHECK(std::fabs(estimateHz(r, 0.55, 0.85) - 660.0) < 6.0);
    }
    {
        // velocity 85 (0.67): linear -> loud layer; curve +1 (v^2 = 0.45 -> 57) -> soft layer.
        const auto lin = renderNotes(samplerPatch(sfz), OfflineRenderer::notesToEvents({{69, 0.0, 0.4, 85}}));
        const auto hard = renderNotes(samplerPatch(sfz, {{"velocity_curve", 1.0f}}),
                                      OfflineRenderer::notesToEvents({{69, 0.0, 0.4, 85}}));
        CHECK(std::fabs(estimateHz(lin, 0.05, 0.35) - 660.0) < 6.0);
        CHECK(std::fabs(estimateHz(hard, 0.05, 0.35) - 440.0) < 6.0);
    }
}

TEST_CASE("sampler: transpose, tune, volume and pan params", "[sampler]") {
    const std::string sfz = fixture().path("map.sfz");
    const auto ev = OfflineRenderer::notesToEvents({{69, 0.0, 0.5, 100}});
    const auto base = renderNotes(samplerPatch(sfz), ev);
    const auto tr = renderNotes(samplerPatch(sfz, {{"transpose", 3.0f}}), ev);
    const auto tune = renderNotes(samplerPatch(sfz, {{"tune", 100.0f}}), ev);
    const auto quiet = renderNotes(samplerPatch(sfz, {{"volume_db", -12.0f}}), ev);
    const auto left = renderNotes(samplerPatch(sfz, {{"pan", -1.0f}}), ev);
    CHECK(std::fabs(estimateHz(tr, 0.1, 0.45) - 523.25) < 6.0);
    CHECK(std::fabs(estimateHz(tune, 0.1, 0.45) - 466.16) < 6.0);
    CHECK(std::fabs((rmsDb(base, 0.1, 0.45) - rmsDb(quiet, 0.1, 0.45)) - 12.0) < 0.5);
    double rightEnergy = 0.0;
    for (size_t i = 0; i < left.right.size(); ++i) rightEnergy += std::fabs(left.right[i]);
    CHECK(rightEnergy < 1e-3);
    CHECK(rmsDb(left, 0.1, 0.45) > -30.0);
}

TEST_CASE("sampler: release triggers, also deferred by the sustain pedal", "[sampler]") {
    // Attack region: 440 Hz sine at its keycenter; release region: 1000 Hz sine (trigger=release).
    const std::string sfz = fixture().path("rel.sfz");
    {
        const auto r = renderNotes(samplerPatch(sfz), OfflineRenderer::notesToEvents({{60, 0.0, 0.3, 100}}), 0.6);
        CHECK(std::fabs(estimateHz(r, 0.05, 0.28) - 440.0) < 6.0);
        CHECK(std::fabs(estimateHz(r, 0.33, 0.6) - 1000.0) < 8.0);
    }
    {
        std::vector<TimedEvent> ev = OfflineRenderer::notesToEvents({{60, 0.05, 0.25, 100}});
        ev.insert(ev.begin(), {0.0, MidiEvent::cc(64, 127)});
        ev.push_back({0.6, MidiEvent::cc(64, 0)});
        const auto r = renderNotes(samplerPatch(sfz), ev, 0.6);
        CHECK(std::fabs(estimateHz(r, 0.35, 0.58) - 440.0) < 6.0); // held by the pedal
        CHECK(std::fabs(estimateHz(r, 0.65, 0.95) - 1000.0) < 8.0); // released at pedal-up
    }
}

TEST_CASE("sampler: notes with a random EG delay always release (sfizz ADSR patch)", "[sampler]") {
    // sfizz 1.2.3 never released a voice whose delay segment ended exactly on a chunk boundary (patched in
    // cmake/Dependencies.cmake). ~2-3 % of delay_random voices hit it, so 300 notes reproduce it reliably.
    SamplerModule m;
    m.loadState({{"sfz", fixture().path("delay.sfz")}});
    m.prepare(kSr, 64);
    REQUIRE(waitReady(m));
    Block b(64);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.numSamples = 64;
    for (int i = 0; i < 300; ++i) {
        const MidiEvent on = MidiEvent::noteOn(40 + i % 40, 100, 1, static_cast<uint32_t>(i % 64));
        const MidiEvent off = MidiEvent::noteOff(40 + i % 40, 0, 1, static_cast<uint32_t>((i * 7) % 64));
        for (int blk = 0; blk < 12; ++blk) {
            b.clear();
            m.process(b.ab, blk == 0 ? MidiEventSpan(&on, 1) : blk == 8 ? MidiEventSpan(&off, 1) : MidiEventSpan(), ctx);
        }
    }
    for (int blk = 0; blk < 1500; ++blk) { // 2 s: every release (10 ms) is long over
        b.clear();
        m.process(b.ab, MidiEventSpan(), ctx);
    }
    CHECK(m.activeVoices() == 0);
    CHECK(b.peak() == 0.0f);
}

TEST_CASE("sampler: loading state, silence while loading, failures", "[sampler]") {
    auto delay = std::make_unique<LoadDelayGuard>(300, 0);
    SamplerModule m;
    m.loadState({{"sfz", fixture().path("map.sfz")}});
    m.prepare(kSr, 64);
    CHECK_FALSE(m.isReady());
    CHECK(m.params().get(SamplerModule::Loading) == 1.0f);
    Block b(64);
    const MidiEvent on = MidiEvent::noteOn(69, 100);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.numSamples = 64;
    b.clear();
    m.process(b.ab, MidiEventSpan(&on, 1), ctx);
    CHECK(b.peak() == 0.0f);
    CHECK(m.params().get(SamplerModule::Loading) == 1.0f);
    REQUIRE(waitReady(m));
    delay.reset();
    CHECK_FALSE(m.loadFailed());
    CHECK(m.regionCount() == 2);
    float peak = 0.0f;
    for (int i = 0; i < 200; ++i) {
        b.clear();
        m.process(b.ab, i == 0 ? MidiEventSpan(&on, 1) : MidiEventSpan(), ctx);
        peak = std::max(peak, b.peak());
    }
    CHECK(peak > 0.05f);
    CHECK(m.params().get(SamplerModule::Loading) == 0.0f);
    CHECK(m.params().get(SamplerModule::LoadedRegions) == 2.0f);

    SamplerModule missing;
    missing.loadState({{"sfz", "assets/samples/does-not-exist/x.sfz"}});
    missing.prepare(kSr, 64);
    CHECK(missing.isReady());
    CHECK(missing.loadFailed());
    CHECK_FALSE(missing.lastError().empty());

    SamplerModule outside;
    outside.loadState({{"sfz", "C:/Windows/win.ini"}});
    outside.prepare(kSr, 64);
    CHECK(outside.loadFailed());
    CHECK(outside.lastError().find("outside") != std::string::npos);

    SamplerModule empty; // no state: ready, silent
    empty.prepare(kSr, 64);
    CHECK(empty.isReady());
    b.clear();
    empty.process(b.ab, MidiEventSpan(&on, 1), ctx);
    CHECK(b.peak() == 0.0f);

    // Destroying a module mid-load must not block on the load; the detached loader frees the Core afterwards.
    {
        const int cores = SamplerModule::liveCores();
        LoadDelayGuard g(0, 600); // the load itself runs, then stalls 600 ms "inside" it
        const auto t0 = std::chrono::steady_clock::now();
        {
            SamplerModule busy;
            busy.loadState({{"sfz", fixture().path("map.sfz")}});
            busy.prepare(kSr, 64);
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // loader is past sfizz_load_file now
        }
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400));
        CHECK(SamplerModule::liveCores() == cores + 1); // still owned by the detached loader
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (SamplerModule::liveCores() > cores && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(SamplerModule::liveCores() == cores);
    }
    // Destroyed before its load starts: skipped, Core freed.
    {
        const int cores = SamplerModule::liveCores();
        const int loads = SamplerModule::totalLoads();
        {
            LoadDelayGuard g(2000, 0);
            SamplerModule queued;
            queued.loadState({{"sfz", fixture().path("map.sfz")}});
            queued.prepare(kSr, 64);
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (SamplerModule::liveCores() > cores && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(SamplerModule::liveCores() == cores);
        CHECK(SamplerModule::totalLoads() == loads);
    }
}

TEST_CASE("sampler: graph rebuild reuses the instance without reloading", "[sampler][graph]") {
    Patch p = samplerPatch(fixture().path("map.sfz"));
    auto b1 = GraphBuilder::build(p, defaultRegistry(), nullptr, kSr, 64);
    Module* m1 = b1.graph->findModule(2);
    REQUIRE(m1 != nullptr);
    REQUIRE(waitReady(*m1));
    const int loads = SamplerModule::totalLoads();

    // Add an FX and change instrument params: the sampler is shared, not rebuilt.
    ModuleSlot fx;
    fx.node = 3;
    fx.type = "gain";
    p.layers[0].fx.push_back(fx);
    p.layers[0].instrument.params["volume_db"] = -6.0f;
    auto b2 = GraphBuilder::build(p, defaultRegistry(), b1.graph.get(), kSr, 64);
    CHECK(b2.graph->findModule(2) == m1);
    CHECK(b2.reused >= 1);
    CHECK(m1->params().get(SamplerModule::VolumeDb) == -6.0f);
    p.layers[0].fx[0].params["gain_db"] = -3.0f;
    auto b3 = GraphBuilder::build(p, defaultRegistry(), b2.graph.get(), kSr, 64);
    CHECK(b3.graph->findModule(2) == m1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(SamplerModule::totalLoads() == loads);

    // A different sfz is a different state -> new instance, one more load.
    p.layers[0].instrument.state = {{"sfz", fixture().path("vel.sfz")}};
    auto b4 = GraphBuilder::build(p, defaultRegistry(), b3.graph.get(), kSr, 64);
    Module* m4 = b4.graph->findModule(2);
    CHECK(m4 != m1);
    REQUIRE(waitReady(*m4));
    CHECK(SamplerModule::totalLoads() == loads + 1);
}

TEST_CASE("sampler: no RT violations, live and offline, incl. polyphony change", "[sampler][rt]") {
    SamplerModule m;
    m.loadState({{"sfz", fixture().path("map.sfz")}});
    m.prepare(kSr, 64);
    REQUIRE(waitReady(m));
    Block b(64);
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.numSamples = 64;
    ChannelState cs;
    ctx.channel = &cs;
    rt::resetViolations();
    int maxVoices = 0;
    for (int blk = 0; blk < 3000; ++blk) {
        std::vector<MidiEvent> ev; // built outside the RT scope
        if (blk % 50 == 0)
            for (int k = 0; k < 8; ++k) ev.push_back(MidiEvent::noteOn(60 + k, 90, 1, static_cast<uint32_t>(k * 7)));
        if (blk % 50 == 25) {
            for (int k = 0; k < 8; ++k) ev.push_back(MidiEvent::noteOff(60 + k, 0, 1, static_cast<uint32_t>(k)));
            ev.push_back(MidiEvent::pitchBend(0.3f, 1, 10));
            ev.push_back(MidiEvent::cc(64, (blk / 50) % 2 ? 127 : 0, 1, 20));
        }
        if (blk == 1000) m.params().set(SamplerModule::Polyphony, 4.0f);
        // Keep rendering in (slowed) real time while the loader pauses the synth and reallocates voices, so the
        // audio thread meets the Paused state mid-stream.
        if (blk > 1000 && blk < 1300) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        b.clear();
        {
            rt::RtScope scope;
            m.process(b.ab, MidiEventSpan(ev.data(), ev.size()), ctx);
        }
        if (blk > 1500) maxVoices = std::max(maxVoices, m.activeVoices());
        if (blk % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // let the loader react
    }
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
    CHECK(maxVoices > 0);
    CHECK(maxVoices <= 8); // polyphony 4 + sfizz overflow voices (x1.5, min +4) for dying notes

    rt::resetViolations();
    const auto r = renderNotes(samplerPatch(fixture().path("map.sfz")),
                               OfflineRenderer::standardTestEvents(), 1.0);
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
    CHECK(rmsDb(r, 0.0, r.seconds()) > -40.0);
}

TEST_CASE("sampler: sample path resolution and allow-list", "[sampler][paths]") {
    const fs::path root = AppPaths::discover().root;
    CHECK(resolveSamplePath(fixture().path("map.sfz"), root).path.has_value());
    CHECK_FALSE(resolveSamplePath("", root).path.has_value());
    CHECK_FALSE(resolveSamplePath("assets/../../outside.sfz", root).path.has_value());
    CHECK_FALSE(resolveSamplePath("assets/samples/../../../x.sfz", root).path.has_value());
    CHECK_FALSE(resolveSamplePath("\\\\server\\share\\x.sfz", root).path.has_value());
}

// Heavy: loads the real libraries (GBs of FLAC/WAV). Run explicitly: ks-tests "[samples]".
TEST_CASE("sampler: factory sampler presets render (installed libraries only)", "[.samples]") {
    const PresetStore store(AppPaths::fromRoot(KS_SOURCE_DIR));
    const auto events = OfflineRenderer::standardTestEvents();
    int rendered = 0, skipped = 0;
    for (const auto& info : store.list()) {
        if (!info.factory) continue;
        const Patch patch = store.load(info.path);
        bool usesSampler = false, available = true;
        for (const Layer& l : patch.layers) {
            if (l.instrument.type != "sampler") continue;
            usesSampler = true;
            const std::string sfz = l.instrument.state.value("sfz", std::string());
            available = available && resolveSamplePath(sfz, AppPaths::discover().root).path.has_value();
        }
        if (!usesSampler) continue;
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
        if (const char* f = std::getenv("KS_SAMPLES_FILTER"); f && *f) { // debug aid: comma-separated substrings
            bool match = false;
            std::string list = f;
            for (size_t pos = 0; pos <= list.size();) {
                const size_t end = std::min(list.find(',', pos), list.size());
                const std::string tok = list.substr(pos, end - pos);
                match = match || (!tok.empty() && info.path.find(tok) != std::string::npos);
                pos = end + 1;
            }
            if (!match) continue;
        }
        if (!available) {
            WARN("skipped (library not installed): " << info.path);
            ++skipped;
            continue;
        }
        INFO("preset " << info.path);
        RenderOptions opt;
        opt.tailSeconds = 3.0;
        rt::resetViolations();
        const auto t0 = std::chrono::steady_clock::now();
        const RenderResult r = OfflineRenderer::render(patch, events, opt);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        float peak = 0.0f;
        bool finite = true;
        double sum = 0.0;
        for (size_t i = 0; i < r.left.size(); ++i)
            for (float x : {r.left[i], r.right[i]}) {
                finite = finite && std::isfinite(x);
                peak = std::max(peak, std::fabs(x));
                sum += x;
            }
        const double dc = sum / (2.0 * static_cast<double>(r.left.size()));
        const double all = rmsDb(r, 0.0, r.seconds()), tail = rmsDb(r, r.seconds() - 0.5, r.seconds());
        std::printf("  %-60s load+render %.2fs  rms %.1f dB  tail %.1f dB  peak %.3f\n", info.path.c_str(), secs,
                    all, tail, peak);
        CAPTURE(peak, dc, all, tail);
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
        if (const char* dump = std::getenv("KS_SAMPLES_DUMP"); dump && *dump) { // debug aid: write every render
            std::string err;
            OfflineRenderer::writeWav(std::string(dump) + "/" + fs::path(info.path).stem().string() + ".wav", r, err);
        }
        CHECK(r.warnings.empty());
        CHECK(finite);
        CHECK(peak <= 1.0f);
        CHECK(std::fabs(dc) < 1e-3);
        // Drum kits: the generic pattern hits many unmapped keys (lower RMS) and one-shot cymbals ring for
        // seconds by design, so they get looser bounds than pitched instruments.
        const bool drums = patch.meta.category == "Drums";
        CHECK(all > (drums ? -50.0 : -40.0));
        CHECK(tail < (drums ? -50.0 : -80.0));
        if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
        ++rendered;
    }
    std::printf("  sampler presets: %d rendered, %d skipped\n", rendered, skipped);
    CHECK(rendered + skipped > 0);
}

#endif // KS_HAS_SFIZZ
