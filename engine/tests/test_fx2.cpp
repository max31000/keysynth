// Effects v2: vocoder (+ the external input bus), wah, echo (Binson Echorec), tapestop. Measurements on the module
// directly at 48 kHz unless noted (ARCHITECTURE §6 "Audio input", §7).

#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/RtCheck.h"
#include "dsp/Denormals.h"
#include "dsp/Svf.h"
#include "effects/echo/EchoFx.h"
#include "effects/tapestop/TapeStopFx.h"
#include "effects/vocoder/VocoderFx.h"
#include "effects/wah/WahFx.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace ks;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;
const char* const kFx2[] = {"vocoder", "wah", "echo", "tapestop"};

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    size_t size() const { return l.size(); }
};

std::unique_ptr<Module> make(const std::string& type, const std::vector<std::pair<const char*, float>>& ps = {},
                             double sr = kSr, int block = kBlock) {
    auto m = defaultRegistry().create(type);
    REQUIRE(m);
    for (const auto& [id, v] : ps) REQUIRE(m->params().set(id, v));
    m->prepare(sr, block);
    return m;
}

void setP(Module& m, const char* id, float v) { REQUIRE(m.params().set(id, v)); }

struct Runner {
    ChannelState cs{};
    ProcessContext ctx;
    explicit Runner(double sr = kSr, double tempo = 120.0) {
        ctx.sampleRate = sr;
        ctx.channel = &cs;
        ctx.transport.tempo = tempo;
    }
    // In place, in blocks; `in` (optional) is the external input bus, `events` are placed by absolute sample.
    void run(Module& m, Stereo& s, const Stereo* in = nullptr, int block = kBlock, size_t from = 0,
             size_t to = SIZE_MAX, const std::vector<std::pair<size_t, MidiEvent>>& events = {}) {
        to = std::min(to, s.size());
        std::vector<MidiEvent> ev;
        for (size_t off = from; off < to; off += static_cast<size_t>(block)) {
            const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), to - off));
            AudioBlock b{s.l.data() + off, s.r.data() + off, n};
            ctx.numSamples = n;
            ctx.input = in ? ExternalInput{in->l.data() + off, in->r.data() + off, 2} : ExternalInput{};
            ev.clear();
            for (const auto& [at, e] : events)
                if (at >= off && at < off + static_cast<size_t>(n)) {
                    MidiEvent x = e;
                    x.sampleOffset = static_cast<uint32_t>(at - off);
                    ev.push_back(x);
                }
            m.process(b, MidiEventSpan(ev.data(), ev.size()), ctx);
            ctx.sampleTime += n;
        }
    }
};

void run(Module& m, Stereo& s, double tempo = 120.0, int block = kBlock, double sr = kSr) {
    Runner r(sr, tempo);
    r.run(m, s, nullptr, block);
}

Stereo sine(double hz, double amp, size_t n, double sr = kSr) {
    Stereo s(n);
    for (size_t i = 0; i < n; ++i) {
        const float v = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
        s.l[i] = s.r[i] = v;
    }
    return s;
}

Stereo noise(size_t n, float amp, uint32_t seed = 1) {
    Stereo s(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s.l[i] = amp * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
        x = x * 1664525u + 1013904223u;
        s.r[i] = amp * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
    }
    return s;
}

// Naive band-limited saw (enough harmonics for a carrier).
Stereo saw(double hz, double amp, size_t n) {
    Stereo s(n);
    double ph = 0.0;
    for (size_t i = 0; i < n; ++i) {
        s.l[i] = s.r[i] = static_cast<float>(amp * (2.0 * ph - 1.0));
        ph += hz / kSr;
        ph -= std::floor(ph);
    }
    return s;
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
    double s = 0.0;
    b = std::min(b, v.size());
    for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
    return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}
double peakAbs(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
    double p = 0.0;
    b = std::min(b, v.size());
    for (size_t i = a; i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
    return p;
}
double maxStep(const std::vector<float>& v, size_t a = 1, size_t b = SIZE_MAX) {
    double p = 0.0;
    b = std::min(b, v.size());
    for (size_t i = std::max<size_t>(a, 1); i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(v[i] - v[i - 1])));
    return p;
}
bool allFinite(const Stereo& s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
    return true;
}
double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// RMS of x[a, b) after a 4th-order band-pass at hz (Q ~ 4): energy "around" a frequency.
double bandRms(const std::vector<float>& x, size_t a, size_t b, double hz) {
    dsp::Svf f1, f2;
    f1.setSampleRate(kSr);
    f2.setSampleRate(kSr);
    const float g = static_cast<float>(std::tan(kPi * hz / kSr));
    f1.setGK(g, 0.25f);
    f2.setGK(g, 0.25f);
    double s = 0.0;
    for (size_t i = 0; i < std::min(b, x.size()); ++i) {
        const float y = 0.25f * f2.tick(0.25f * f1.tick(x[i]).bp).bp;
        if (i >= a) s += static_cast<double>(y) * y;
    }
    return std::sqrt(s / static_cast<double>(b - a));
}

// Zero-crossing frequency of x[a, b).
double zcFreq(const std::vector<float>& x, size_t a, size_t b) {
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            if (crossings == 0) first = i;
            last = i;
            ++crossings;
        }
    if (crossings < 2) return 0.0;
    return (crossings - 1) * kSr / static_cast<double>(last - first);
}

} // namespace

// --------------------------------------------------------------------------------------------- common

TEST_CASE("FX2: registered, kind Effect, zero latency, mix 0 bit-transparent", "[fx2]") {
    const Stereo src = noise(24000, 0.5f, 3);
    for (const char* t : kFx2) {
        INFO(t);
        const ModuleInfo* info = defaultRegistry().find(t);
        REQUIRE(info);
        REQUIRE(info->kind == ModuleKind::Effect);
        auto m = make(t, {{"mix", 0.0f}});
        REQUIRE(m->latencySamples() == 0);
        REQUIRE(m->tailSamples() >= 0);
        Stereo s = src;
        run(*m, s);
        REQUIRE(s.l == src.l);
        REQUIRE(s.r == src.r);
    }
    // tapestop running (not stopped) at full mix is the dry signal, bit for bit.
    auto ts = make("tapestop");
    Stereo s = src;
    run(*ts, s);
    REQUIRE(s.l == src.l);
}

TEST_CASE("FX2: bounded and finite under extreme params, rates and block sizes", "[fx2][stress]") {
    const Stereo src = [] {
        Stereo s = noise(48000, 0.8f, 7);
        for (size_t i = 0; i < s.size(); i += 4800) s.l[i] = s.r[i] = 1.0f;
        return s;
    }();
    const Stereo mic = noise(48000, 0.9f, 99);
    uint32_t rng = 777;
    for (const char* t : kFx2) {
        auto m = make(t);
        const auto& specs = m->params().specs();
        for (int pass = 0; pass < 12; ++pass) {
            for (int p = 0; p < static_cast<int>(specs.size()); ++p) {
                const ParamSpec& sp = specs[static_cast<size_t>(p)];
                if (sp.isReadOnly()) continue;
                rng = rng * 1664525u + 1013904223u;
                const bool hi = pass == 1 || sp.id == "mix" || (pass >= 2 && (rng >> 16) & 1);
                m->params().set(p, hi ? sp.max : sp.min);
            }
            m->reset();
            Stereo s = src;
            Runner r;
            r.run(*m, s, pass % 2 ? &mic : nullptr);
            INFO(t << " pass " << pass);
            REQUIRE(allFinite(s));
            REQUIRE(peakAbs(s.l) < 64.0);
            REQUIRE(peakAbs(s.r) < 64.0);
        }
    }
    for (double sr : {44100.0, 96000.0, 192000.0})
        for (int block : {1, 32, 512})
            for (const char* t : kFx2) {
                auto m = make(t, {{"mix", 1.0f}}, sr, block);
                Stereo s = noise(static_cast<size_t>(sr / 4), 0.5f, 21);
                Runner r(sr);
                r.run(*m, s, nullptr, block);
                INFO(t << " @ " << sr << " / " << block);
                REQUIRE(allFinite(s));
                REQUIRE(peakAbs(s.l) < 8.0);
            }
    // Echo swell at 100 % with loud sustained input stays bounded for 20 s (runaway is soft-limited).
    auto e = make("echo", {{"feedback", 1.0f}, {"mix", 1.0f}, {"time", 30.0f}, {"tone", 1.0f}, {"drive", 1.0f}});
    Stereo s = sine(700.0, 1.0, static_cast<size_t>(20 * kSr));
    run(*e, s);
    REQUIRE(allFinite(s));
    REQUIRE(peakAbs(s.l) < 4.0);
}

TEST_CASE("FX2: tails are honest; silence costs nothing", "[fx2]") {
    struct Case {
        const char* type;
        std::vector<std::pair<const char*, float>> ps;
    };
    const Case cases[] = {
        {"vocoder", {{"modulator", 1.0f}, {"sibilance", 1.0f}, {"bands", 0.0f}, {"formant", -12.0f}}},
        {"vocoder", {{"modulator", 2.0f}, {"bands", 3.0f}, {"carrier_mix", 1.0f}}},
        {"wah", {{"q", 20.0f}, {"pedal", 0.0f}, {"range", 1.5f}}},
        {"wah", {{"mode", 1.0f}, {"release_ms", 2000.0f}}},
        {"echo", {}},
        {"echo", {{"feedback", 0.9f}, {"heads", 3.0f}, {"time", 400.0f}}},
        {"tapestop", {}},
        {"tapestop", {{"stop_ms", 4000.0f}, {"start_ms", 4000.0f}}},
    };
    for (const Case& c : cases) {
        auto m = make(c.type, c.ps);
        setP(*m, "mix", 1.0f);
        m->reset();
        const size_t burst = 24000, tail = static_cast<size_t>(m->tailSamples()), total = burst + tail + 24000;
        Stereo s(total);
        const Stereo nz = noise(burst, 0.7f, 13);
        std::copy(nz.l.begin(), nz.l.end(), s.l.begin());
        std::copy(nz.r.begin(), nz.r.end(), s.r.begin());
        run(*m, s);
        INFO(c.type << " tail " << tail);
        REQUIRE(rms(s.l, burst + tail, total) < 1e-4);
        REQUIRE(rms(s.r, burst + tail, total) < 1e-4);
    }
    // A tape stop in flight when the input ends: the ramp's resampled audio is inside the tail too.
    {
        auto m = make("tapestop", {{"stop_ms", 4000.0f}, {"start_ms", 4000.0f}});
        Stereo s(static_cast<size_t>(kSr) + static_cast<size_t>(m->tailSamples()) + 24000);
        const Stereo nz = noise(static_cast<size_t>(kSr), 0.7f, 5);
        std::copy(nz.l.begin(), nz.l.end(), s.l.begin());
        Runner r;
        r.run(*m, s, nullptr, kBlock, 0, 24000);
        setP(*m, "stop", 1.0f);
        r.run(*m, s, nullptr, kBlock, 24000, 48000);
        setP(*m, "stop", 0.0f);
        r.run(*m, s, nullptr, kBlock, 48000);
        const size_t end = 48000 + static_cast<size_t>(m->tailSamples());
        REQUIRE(rms(s.l, end, s.size()) < 1e-4);
    }
    const dsp::ScopedNoDenormals ftz;
    for (const char* t : kFx2) {
        auto m = make(t, {{"mix", 1.0f}});
        Stereo active = noise(48000, 0.5f, 5);
        const auto t0 = std::chrono::steady_clock::now();
        run(*m, active);
        const double activeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Stereo silent(static_cast<size_t>(10 * kSr));
        const auto t1 = std::chrono::steady_clock::now();
        run(*m, silent);
        const double silentSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() / 10.0;
        INFO(t << " active " << activeSec * 1e3 << " ms/s, silent " << silentSec * 1e3 << " ms/s");
        REQUIRE(allFinite(silent));
        REQUIRE(silentSec < 3.0 * activeSec + 2e-3);
    }
}

TEST_CASE("FX2: no RT violations (with an input bus)", "[fx2][rt]") {
    std::vector<std::unique_ptr<Module>> chain;
    for (const char* t : kFx2) chain.push_back(make(t));
    setP(*chain[3], "stop", 1.0f);
    Stereo s = noise(static_cast<size_t>(kSr), 0.3f, 2);
    const Stereo mic = noise(static_cast<size_t>(kSr), 0.3f, 9);
    const uint64_t before = rt::violationCount();
    {
        rt::RtScope scope;
        Runner r;
        for (auto& m : chain) r.run(*m, s, &mic);
    }
    REQUIRE(rt::violationCount() == before);
    REQUIRE(allFinite(s));
}

// --------------------------------------------------------------------------------------------- input bus

TEST_CASE("Engine: device input becomes the external input bus (mono -> both sides), metered", "[fx2][audio][engine]") {
    Engine e;
    e.prepare(48000.0, 64);
    std::vector<float> in(256), l(256), r(256);
    for (size_t i = 0; i < in.size(); ++i) in[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
    const float* ins[1] = {in.data()};
    float* outs[2] = {l.data(), r.data()};
    e.process(ins, 1, outs, 2, 256);
    REQUIRE(std::fabs(e.telemetry().inputL.load() - 0.5f) < 0.01f);
    REQUIRE(e.telemetry().inputR.load() == e.telemetry().inputL.load());
    e.telemetry().inputL.store(0.0f);
    e.process(outs, 2, 256); // no input
    REQUIRE(e.telemetry().inputL.load() == 0.0f);

    // ExternalInput::offset follows the chunking.
    const ExternalInput x{in.data(), in.data(), 1};
    REQUIRE(x.offset(64).left == in.data() + 64);
    REQUIRE(!ExternalInput{}.offset(64).present());
}

// --------------------------------------------------------------------------------------------- vocoder

TEST_CASE("Vocoder: band layout and band energy follows the modulator spectrum", "[fx2][vocoder]") {
    for (int bi = 0; bi < 4; ++bi) {
        auto m = make("vocoder", {{"bands", static_cast<float>(bi)}});
        auto& v = dynamic_cast<VocoderFx&>(*m);
        const int n = VocoderFx::bandCount(bi);
        REQUIRE(v.activeBands() == n);
        REQUIRE(std::fabs(v.bandHz(0) - VocoderFx::kLowHz) < 0.5f);
        REQUIRE(std::fabs(v.bandHz(n - 1) - VocoderFx::kHighHz) / VocoderFx::kHighHz < 0.01f);
    }
    const size_t n = static_cast<size_t>(0.8 * kSr), from = static_cast<size_t>(0.4 * kSr);
    const Stereo carrier = noise(n, 0.3f, 4);
    for (double modHz : {400.0, 1000.0, 3000.0}) {
        auto m = make("vocoder", {{"bands", 3.0f}, {"sibilance", 0.0f}});
        auto& v = dynamic_cast<VocoderFx&>(*m);
        const Stereo mic = sine(modHz, 0.25, n);
        Stereo s = carrier;
        Runner r;
        r.run(*m, s, &mic);
        // Strongest band = the one at the modulator frequency.
        int best = 0;
        for (int b = 1; b < v.activeBands(); ++b)
            if (v.bandEnvelope(b) > v.bandEnvelope(best)) best = b;
        INFO("mod " << modHz << " best band " << best << " @ " << v.bandHz(best));
        REQUIRE(std::fabs(std::log2(v.bandHz(best) / modHz)) < 0.25);
        // Output energy concentrates there: >= 15 dB above an octave and a half away on either side.
        const double at = bandRms(s.l, from, n, modHz);
        REQUIRE(db(at / bandRms(s.l, from, n, modHz * 2.83)) > 15.0);
        REQUIRE(db(at / bandRms(s.l, from, n, modHz / 2.83)) > 15.0);
    }
    // Formant shift +12 st moves the output an octave up.
    {
        auto m = make("vocoder", {{"bands", 3.0f}, {"sibilance", 0.0f}, {"formant", 12.0f}});
        const Stereo mic = sine(800.0, 0.25, n);
        Stereo s = carrier;
        Runner r;
        r.run(*m, s, &mic);
        REQUIRE(db(bandRms(s.l, from, n, 1600.0) / bandRms(s.l, from, n, 800.0)) > 10.0);
    }
    // Input channel selection: Right picks the right side of the bus only.
    {
        auto m = make("vocoder", {{"input_channel", 1.0f}, {"sibilance", 0.0f}});
        Stereo mic(n);
        const Stereo tone = sine(1000.0, 0.25, n);
        mic.l = tone.l; // tone on the left, silence on the right
        Stereo s = carrier;
        Runner r;
        r.run(*m, s, &mic);
        REQUIRE(rms(s.l, from, n) < 1e-3);
    }
    // Gate closes on a quiet modulator; a present but silent input stays silent (no fallback).
    {
        auto m = make("vocoder", {{"gate_db", -30.0f}});
        const Stereo mic = noise(n, 0.005f, 3); // -46 dBFS
        Stereo s = carrier;
        Runner r;
        r.run(*m, s, &mic);
        REQUIRE(rms(s.l, from, n) < 1e-3);
    }
    // No input channel open: `Input` falls back to the internal vowels (audible offline).
    {
        auto m = make("vocoder");
        Stereo s = saw(110.0, 0.3, n);
        const double carrierRms = rms(s.l, from, n);
        run(*m, s);
        // the wet is in the carrier's level range (unity-ish band gain calibration)
        UNSCOPED_INFO("vowels wet " << db(rms(s.l, 0, n) / carrierRms) << " dB re carrier");
        REQUIRE(std::fabs(db(rms(s.l, 0, n) / carrierRms)) < 9.0);
    }
}

TEST_CASE("Vocoder: sibilance adds HF noise on consonants, only while the carrier sounds", "[fx2][vocoder]") {
    const size_t n = static_cast<size_t>(0.6 * kSr), from = static_cast<size_t>(0.3 * kSr);
    // "sss": modulator = high-passed noise; carrier = a low saw (no energy up there of its own).
    Stereo mic = noise(n, 0.3f, 8);
    {
        dsp::Svf hp;
        hp.setSampleRate(kSr);
        hp.setCutoff(7000.0f, 0.0f);
        for (size_t i = 0; i < n; ++i) mic.l[i] = mic.r[i] = hp.process(mic.l[i], dsp::Svf::Mode::HighPass);
    }
    auto hfOut = [&](float sib, bool carrierOn) {
        auto m = make("vocoder", {{"sibilance", sib}});
        Stereo s = carrierOn ? saw(55.0, 0.3, n) : Stereo(n);
        Runner r;
        r.run(*m, s, &mic);
        return bandRms(s.l, from, n, 8000.0);
    };
    REQUIRE(db(hfOut(1.0f, true) / hfOut(0.0f, true)) > 12.0);
    REQUIRE(hfOut(1.0f, false) < 1e-5);
}

// --------------------------------------------------------------------------------------------- wah

TEST_CASE("Wah: centre frequency follows the pedal (heel..toe), model curves, sources", "[fx2][wah]") {
    auto peakHz = [](Module& m, float pedal) {
        setP(m, "pedal", pedal);
        double bestHz = 0.0, best = 0.0;
        for (double hz = 150.0; hz < 6000.0; hz *= 1.03) {
            m.reset();
            Stereo s = sine(hz, 0.1, 4800);
            run(m, s);
            const double g = rms(s.l, 2400, 4800);
            if (g > best) {
                best = g;
                bestHz = hz;
            }
        }
        return bestHz;
    };
    for (int model : {0, 1}) {
        auto m = make("wah", {{"model", static_cast<float>(model)}});
        auto& w = dynamic_cast<WahFx&>(*m);
        double prev = 0.0;
        for (float pedal : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            const double hz = peakHz(*m, pedal);
            INFO("model " << model << " pedal " << pedal << " peak " << hz << " expected " << w.centreHz(pedal));
            REQUIRE(std::fabs(std::log2(hz / w.centreHz(pedal))) < 0.1);
            REQUIRE(hz > prev);
            prev = hz;
        }
    }
    auto m = make("wah");
    auto& w = dynamic_cast<WahFx&>(*m);
    REQUIRE(std::fabs(w.centreHz(0.0f) - 350.0f) < 5.0f);  // Cry Baby heel
    REQUIRE(std::fabs(w.centreHz(1.0f) - 2200.0f) < 20.0f); // toe
    setP(*m, "range", 0.5f);
    REQUIRE(w.centreHz(1.0f) / w.centreHz(0.0f) < 2.6f); // narrower sweep

    // Mod wheel / CC11 drive the pedal in Manual mode.
    auto mw = make("wah", {{"pedal_source", 1.0f}, {"pedal", 0.0f}});
    Runner r;
    r.cs.modWheel = 1.0f;
    Stereo s = noise(4800, 0.2f, 1);
    r.run(*mw, s);
    REQUIRE(dynamic_cast<WahFx&>(*mw).lastPosition() > 0.99f);
    auto ex = make("wah", {{"pedal_source", 2.0f}, {"pedal", 1.0f}});
    r.cs.expression = 0.0f;
    s = noise(4800, 0.2f, 1);
    r.run(*ex, s);
    REQUIRE(dynamic_cast<WahFx&>(*ex).lastPosition() < 0.01f);

    // Auto: a loud input opens the pedal, silence closes it again.
    auto au = make("wah", {{"mode", 1.0f}, {"pedal", 0.1f}, {"sens", 1.0f}});
    Stereo loud = sine(300.0, 0.6, 9600);
    run(*au, loud);
    REQUIRE(dynamic_cast<WahFx&>(*au).lastPosition() > 0.6f);
    Stereo quiet(48000);
    run(*au, quiet);
    REQUIRE(dynamic_cast<WahFx&>(*au).lastPosition() < 0.15f);

    // LFO sweeps the full range at depth 1 (synced 1/4 at 120 bpm = 2 Hz).
    auto lf = make("wah", {{"mode", 2.0f}, {"sync", 7.0f}, {"depth", 1.0f}});
    float lo = 1.0f, hi = 0.0f;
    Stereo blk(64);
    for (int b = 0; b < 1500; ++b) {
        run(*lf, blk);
        lo = std::min(lo, dynamic_cast<WahFx&>(*lf).lastPosition());
        hi = std::max(hi, dynamic_cast<WahFx&>(*lf).lastPosition());
    }
    REQUIRE(lo < 0.05f);
    REQUIRE(hi > 0.95f);
}

// --------------------------------------------------------------------------------------------- echo

TEST_CASE("Echo: tap times per heads setting, sync, swell regeneration", "[fx2][echo]") {
    auto tapsOf = [](int heads, float timeMs, float fb, float syncIdx = 0.0f) {
        auto m = make("echo", {{"heads", static_cast<float>(heads)}, {"time", timeMs}, {"sync", syncIdx},
                               {"feedback", fb}, {"wow", 0.0f}, {"flutter", 0.0f}, {"drive", 0.0f},
                               {"width", 0.0f}, {"tone", 1.0f}, {"mix", 1.0f}});
        Stereo s(static_cast<size_t>(2.5 * kSr));
        s.l[0] = s.r[0] = 0.5f;
        run(*m, s);
        return s;
    };
    auto energyAt = [](const Stereo& s, double sec) {
        const size_t c = static_cast<size_t>(sec * kSr);
        return peakAbs(s.l, c - 96, c + 96);
    };
    for (int heads = 0; heads < EchoFx::kNumCombos; ++heads) {
        const unsigned mask = EchoFx::headMask(heads);
        const Stereo s = tapsOf(heads, 100.0f, 0.0f);
        double ref = 0.0;
        for (int k = 0; k < 4; ++k)
            if ((mask >> k) & 1u) ref = std::max(ref, energyAt(s, 0.1 * (k + 1)));
        REQUIRE(ref > 0.01);
        for (int k = 0; k < 4; ++k) {
            const double e = energyAt(s, 0.1 * (k + 1));
            INFO("heads " << heads << " head " << k + 1 << " level " << db(e / ref));
            if ((mask >> k) & 1u) REQUIRE(db(e / ref) > -3.0);
            else REQUIRE(db(e / ref) < -50.0);
        }
        // nothing before the first selected head, nothing after the last one without swell
        REQUIRE(peakAbs(s.l, 10, 4000) < 1e-4);
        REQUIRE(peakAbs(s.l, static_cast<size_t>(0.45 * kSr), s.size()) < 1e-4);
    }
    // sync 1/8 at 120 bpm = 250 ms spacing: head 2 at 500 ms.
    {
        const Stereo s = tapsOf(1, 100.0f, 0.0f, 10.0f);
        REQUIRE(energyAt(s, 0.5) > 0.01);
        REQUIRE(energyAt(s, 0.2) < 1e-4);
    }
    // A tempo change glides the drum speed to the end without a snap (a float one-pole would stall a few samples
    // short of the target and jump every head: a click).
    {
        auto m = make("echo", {{"sync", 10.0f}, {"heads", 0.0f}, {"feedback", 0.0f}, {"wow", 0.0f},
                               {"flutter", 0.0f}, {"mix", 1.0f}, {"tone", 1.0f}});
        Stereo s = sine(200.0, 0.5, static_cast<size_t>(3 * kSr));
        const double ref = maxStep(s.l);
        Runner r(kSr, 128.0); // reset() assumed 120 bpm
        r.run(*m, s);
        REQUIRE(maxStep(s.l, static_cast<size_t>(0.3 * kSr)) < 1.3 * ref);
    }
    // Same for `delay` (dsp::OnePoleSmoother keeps converging on large targets): a time change glides click-free.
    {
        auto m = make("delay", {{"time", 375.0f}, {"feedback", 0.0f}, {"mix", 1.0f}, {"high_cut_hz", 20000.0f},
                                {"low_cut_hz", 20.0f}});
        Stereo s = sine(200.0, 0.5, static_cast<size_t>(4 * kSr));
        const double ref = maxStep(s.l);
        Runner r;
        r.run(*m, s, nullptr, kBlock, 0, static_cast<size_t>(kSr));
        setP(*m, "time", 350.0f);
        r.run(*m, s, nullptr, kBlock, static_cast<size_t>(kSr));
        REQUIRE(maxStep(s.l, static_cast<size_t>(0.5 * kSr)) < 1.5 * ref);
    }
    // Swell: head 1 regenerates at multiples of the spacing, each repeat quieter.
    {
        const Stereo s = tapsOf(0, 120.0f, 0.6f);
        const double e1 = energyAt(s, 0.12), e2 = energyAt(s, 0.24), e3 = energyAt(s, 0.36);
        REQUIRE(e2 > 0.3 * e1);
        REQUIRE(e2 < e1);
        REQUIRE(e3 < e2);
    }
}

// --------------------------------------------------------------------------------------------- tapestop

TEST_CASE("Tape stop: pitch follows the speed ramp down to zero, then silence; start returns to dry", "[fx2][tapestop]") {
    const float stopMs = 800.0f, startMs = 400.0f;
    auto m = make("tapestop", {{"stop_ms", stopMs}, {"start_ms", startMs}, {"curve", 0.0f}});
    auto& ts = dynamic_cast<TapeStopFx&>(*m);
    const size_t t0 = 24000; // stop pressed at 0.5 s
    const size_t total = static_cast<size_t>(3.0 * kSr);
    const Stereo in = sine(1000.0, 0.5, total);
    Stereo s = in;
    Runner r;
    r.run(*m, s, nullptr, kBlock, 0, t0);
    setP(*m, "stop", 1.0f);
    const size_t stopEnd = t0 + static_cast<size_t>(stopMs * 0.001 * kSr);
    r.run(*m, s, nullptr, kBlock, t0, stopEnd + 4800);
    REQUIRE(ts.speed() == 0.0f);
    // Linear ramp: v(u) = 1 - u -> f = 1000 (1 - u). Measure in 40 ms windows.
    for (double u : {0.1, 0.3, 0.5, 0.7}) {
        const size_t c = t0 + static_cast<size_t>(u * stopMs * 0.001 * kSr);
        const double f = zcFreq(s.l, c - 960, c + 960);
        INFO("u " << u << " f " << f);
        REQUIRE(std::fabs(f - 1000.0 * (1.0 - u)) < 0.06 * 1000.0 * (1.0 - u) + 10.0);
    }
    REQUIRE(peakAbs(s.l, stopEnd + 960, stopEnd + 4800) < 1e-5); // stopped: silence (after the 2 ms gain slew)
    // No clicks on the way down: steps never exceed the dry sine's.
    const double dryStep = maxStep(in.l);
    REQUIRE(maxStep(s.l, t0 - 100, stopEnd + 4800) <= dryStep * 1.05);

    // Start: spins up, then the dry input exactly.
    const size_t t1 = stopEnd + 4800;
    setP(*m, "stop", 0.0f);
    const size_t startEnd = t1 + static_cast<size_t>(startMs * 0.001 * kSr) + static_cast<size_t>(0.012 * kSr);
    r.run(*m, s, nullptr, kBlock, t1, total);
    REQUIRE(ts.running());
    REQUIRE(maxStep(s.l, t1 - 10, total) <= dryStep * 1.1);
    for (size_t i = startEnd + kBlock; i < total; ++i) REQUIRE(s.l[i] == in.l[i]);
    // mid-start the pitch is rising (below 1 kHz)
    const size_t mid = t1 + static_cast<size_t>(0.5 * startMs * 0.001 * kSr);
    REQUIRE(zcFreq(s.l, mid - 960, mid + 960) < 900.0);
}

TEST_CASE("Tape stop: MIDI note / CC triggers, extremes and rapid toggling stay click-free and finite", "[fx2][tapestop]") {
    const size_t n = static_cast<size_t>(2.0 * kSr);
    const Stereo in = sine(440.0, 0.8, n);
    const double dryStep = maxStep(in.l);
    // Note trigger (sample accurate): note 24 held 0.5 .. 1.2 s.
    {
        auto m = make("tapestop", {{"trigger", 1.0f}, {"trigger_note", 24.0f}, {"stop_ms", 300.0f}});
        auto& ts = dynamic_cast<TapeStopFx&>(*m);
        Stereo s = in;
        Runner r;
        r.run(*m, s, nullptr, kBlock, 0, 30000, {{24005, MidiEvent::noteOn(60, 100)}, {24010, MidiEvent::noteOn(24, 100)}});
        REQUIRE(ts.speed() < 0.9f);
        REQUIRE(s.l[24009] == in.l[24009]); // before the trigger sample: dry
        r.run(*m, s, nullptr, kBlock, 30000, n, {{57600, MidiEvent::noteOff(24)}});
        REQUIRE(ts.running());
        REQUIRE(allFinite(s));
    }
    // CC trigger >= 64 stops, < 64 starts.
    {
        auto m = make("tapestop", {{"trigger", 2.0f}, {"trigger_cc", 80.0f}, {"stop_ms", 100.0f}});
        auto& ts = dynamic_cast<TapeStopFx&>(*m);
        Stereo s = in;
        Runner r;
        r.run(*m, s, nullptr, kBlock, 0, 24000, {{1000, MidiEvent::cc(80, 127)}});
        REQUIRE(ts.speed() == 0.0f);
        r.run(*m, s, nullptr, kBlock, 24000, n, {{24000, MidiEvent::cc(80, 0)}});
        REQUIRE(ts.running());
    }
    // Extremes: shortest ramps, both curve ends, toggled every 7 / 23 / 101 blocks.
    for (float curve : {-1.0f, 0.0f, 1.0f})
        for (int every : {7, 23, 101}) {
            auto m = make("tapestop", {{"stop_ms", 20.0f}, {"start_ms", 20.0f}, {"curve", curve}});
            Stereo s = in;
            Runner r;
            bool stop = false;
            for (size_t off = 0, b = 0; off < n; off += kBlock, ++b) {
                if (b % static_cast<size_t>(every) == 0) {
                    stop = !stop;
                    setP(*m, "stop", stop ? 1.0f : 0.0f);
                }
                r.run(*m, s, nullptr, kBlock, off, off + kBlock);
            }
            INFO("curve " << curve << " every " << every);
            REQUIRE(allFinite(s));
            REQUIRE(peakAbs(s.l) <= 0.8 * 1.001);
            // 20 ms ramps and a 10 ms crossfade: steeper than the dry sine but no clicks (a click is ~2x amp).
            REQUIRE(maxStep(s.l) < dryStep * 3.0);
        }
}
