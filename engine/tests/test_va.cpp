// Tests for the `va` virtual analog instrument and its DSP primitives (BlepOsc, SuperSaw, LadderFilter,
// AnalogEnv, Lfo helpers). Module-level tests drive VaSynth directly (no graph) inside an RtScope.

#include "core/ChannelState.h"
#include "core/RtCheck.h"
#include "dsp/AnalogEnv.h"
#include "dsp/BlepOsc.h"
#include "dsp/LadderFilter.h"
#include "dsp/Lfo.h"
#include "instruments/va/VaSynth.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

using namespace ks;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 64;

struct Rig {
    VaSynth synth;
    ChannelState channel;
    std::vector<float> left, right;
    int64_t time = 0;

    explicit Rig(double sr = kSr) { synth.prepare(sr, kBlock); }
    void set(const char* id, float v) { REQUIRE(synth.params().set(id, v)); }

    // Render `seconds`, delivering `events` (offsets relative to the start of this call, in samples).
    void run(double seconds, std::vector<MidiEvent> events = {}) {
        const int total = static_cast<int>(seconds * kSr);
        std::sort(events.begin(), events.end(),
                  [](const MidiEvent& a, const MidiEvent& b) { return a.sampleOffset < b.sampleOffset; });
        std::vector<float> l(kBlock), r(kBlock);
        std::vector<MidiEvent> blockEvents;
        blockEvents.reserve(events.size());
        size_t ei = 0;
        for (int pos = 0; pos < total; pos += kBlock) {
            const int n = std::min(kBlock, total - pos);
            blockEvents.clear();
            while (ei < events.size() && static_cast<int>(events[ei].sampleOffset) < pos + n) {
                MidiEvent e = events[ei++];
                e.sampleOffset = static_cast<uint32_t>(std::max(0, static_cast<int>(e.sampleOffset) - pos));
                if (e.type == MidiEventType::ControlChange || e.type == MidiEventType::PitchBend ||
                    e.type == MidiEventType::ChannelPressure)
                    channel.apply(e);
                blockEvents.push_back(e);
            }
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            AudioBlock b{l.data(), r.data(), n};
            ProcessContext ctx;
            ctx.sampleRate = kSr;
            ctx.numSamples = n;
            ctx.sampleTime = time;
            ctx.channel = &channel;
            {
                rt::RtScope scope;
                synth.process(b, MidiEventSpan(blockEvents.data(), blockEvents.size()), ctx);
            }
            left.insert(left.end(), l.begin(), l.begin() + n);
            right.insert(right.end(), r.begin(), r.begin() + n);
            time += n;
        }
    }
    void clear() {
        left.clear();
        right.clear();
    }
};

uint32_t at(double s) { return static_cast<uint32_t>(s * kSr); }

// Autocorrelation pitch estimate (normalised, first strong peak, parabolic interpolation).
double pitchOf(const std::vector<float>& x, size_t a, size_t b, double minHz = 40.0, double maxHz = 3000.0) {
    const int minLag = static_cast<int>(kSr / maxHz), maxLag = static_cast<int>(kSr / minHz);
    const size_t n = b - a - static_cast<size_t>(maxLag);
    std::vector<double> r(static_cast<size_t>(maxLag + 2), 0.0);
    double r0 = 0.0;
    for (size_t i = 0; i < n; ++i) r0 += static_cast<double>(x[a + i]) * x[a + i];
    for (int lag = minLag; lag <= maxLag + 1; ++lag) {
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[a + i]) * x[a + i + static_cast<size_t>(lag)];
        r[static_cast<size_t>(lag)] = s / r0;
    }
    double best = -1.0;
    for (int lag = minLag; lag <= maxLag; ++lag) best = std::max(best, r[static_cast<size_t>(lag)]);
    for (int lag = minLag + 1; lag <= maxLag; ++lag) {
        const double c = r[static_cast<size_t>(lag)];
        if (c >= 0.9 * best && c >= r[static_cast<size_t>(lag - 1)] && c >= r[static_cast<size_t>(lag + 1)]) {
            const double y0 = r[static_cast<size_t>(lag - 1)], y2 = r[static_cast<size_t>(lag + 1)];
            const double den = y0 - 2.0 * c + y2;
            const double off = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
            return kSr / (lag + off);
        }
    }
    return 0.0;
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Ratio (dB) of spectral energy away from the harmonics of f0 to the energy at the harmonics.
double inharmonicDb(const std::vector<float>& x, size_t start, double f0) {
    const size_t n = 32768;
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / (n - 1));
        a[i] = x[start + i] * w;
    }
    fft(a);
    const double binHz = kSr / static_cast<double>(n);
    double harm = 0.0, other = 0.0;
    for (size_t k = static_cast<size_t>(30.0 / binHz); k < n / 2; ++k) {
        const double f = static_cast<double>(k) * binHz;
        const double h = f / f0;
        const double dist = std::fabs(h - std::round(h)) * f0; // Hz to the nearest harmonic
        const double e = std::norm(a[k]);
        (dist < 6.0 * binHz && std::round(h) >= 1.0 ? harm : other) += e;
    }
    return 10.0 * std::log10(other / std::max(harm, 1e-30));
}

struct Stats {
    bool finite = true;
    float peak = 0.0f;
    double rms = 0.0;
};

Stats stats(const std::vector<float>& x, size_t a = 0, size_t b = ~size_t{0}) {
    Stats s;
    b = std::min(b, x.size());
    double sq = 0.0;
    for (size_t i = a; i < b; ++i) {
        if (!std::isfinite(x[i])) {
            s.finite = false;
            continue;
        }
        s.peak = std::max(s.peak, std::fabs(x[i]));
        sq += static_cast<double>(x[i]) * x[i];
    }
    s.rms = b > a ? std::sqrt(sq / static_cast<double>(b - a)) : 0.0;
    return s;
}

void cleanOsc(Rig& r) {
    r.set("drift", 0.0f);
    r.set("cutoff", 20000.0f);
    r.set("filter_model", 2.0f); // SEM 12 dB: transparent at 20 kHz
    r.set("key_track", 0.0f);
    r.set("amp_vel", 0.0f);
}

} // namespace

// ---------------------------------------------------------------------------------------- primitives

TEST_CASE("BlepOsc: waveforms band-limited, no DC, bounded", "[dsp][va]") {
    for (auto w : {dsp::OscWave::Saw, dsp::OscWave::Pulse, dsp::OscWave::Triangle, dsp::OscWave::Sine}) {
        dsp::BlepOsc o;
        double sum = 0.0;
        float peak = 0.0f;
        const float inc = 440.0f / 48000.0f;
        for (int i = 0; i < 48000; ++i) {
            const float x = o.tick(w, inc, 0.3f);
            sum += x;
            peak = std::max(peak, std::fabs(x));
        }
        INFO("wave " << static_cast<int>(w));
        REQUIRE(peak < 1.6f); // pulse at pw .3 is DC-compensated: +-1 -0.4 offset, plus BLEP ripple
        REQUIRE(std::fabs(sum / 48000.0) < 0.01);
    }
}

TEST_CASE("BlepOsc: hard sync locks slave period to master", "[dsp][va]") {
    dsp::BlepOsc master, slave;
    int wraps = 0;
    std::vector<float> y;
    for (int i = 0; i < 48000; ++i) {
        master.tick(dsp::OscWave::Saw, 200.0f / 48000.0f);
        y.push_back(slave.tick(dsp::OscWave::Saw, 730.0f / 48000.0f, 0.5f, master.wrapD()));
        if (master.wrapD() >= 0.0f) ++wraps;
    }
    REQUIRE(std::abs(wraps - 200) <= 1);
    const double hz = pitchOf(y, 4800, 4800 + 9600, 60.0, 2000.0);
    REQUIRE(std::fabs(hz - 200.0) < 1.0);
}

TEST_CASE("SuperSaw: detune curve monotonic and bounded", "[dsp][va]") {
    float prev = -1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float d = dsp::SuperSaw::detuneCurve(static_cast<float>(i) / 100.0f);
        REQUIRE(d >= prev - 1e-4f);
        REQUIRE(d < 1.1f);
        prev = d;
    }
}

TEST_CASE("LadderFilter: self-oscillation is stable and at the cutoff", "[dsp][va]") {
    for (auto m : {dsp::LadderFilter::Model::Transistor, dsp::LadderFilter::Model::Ota}) {
        dsp::LadderFilter f;
        const float g = dsp::LadderFilter::coefG(1000.0f, 48000.0f);
        const float k = dsp::LadderFilter::feedbackFor(1.0f, m);
        std::vector<float> y;
        float peak = 0.0f;
        for (int i = 0; i < 96000; ++i) {
            const float x = i == 0 ? 0.5f : 0.0f;
            const float v = f.process(x, g, k, m, dsp::LadderFilter::Response::LowPass);
            REQUIRE(std::isfinite(v));
            peak = std::max(peak, std::fabs(v));
            y.push_back(v);
        }
        INFO("model " << static_cast<int>(m));
        const Stats s = stats(y, 48000, 96000);
        REQUIRE(s.rms > 0.05);  // sustained oscillation
        REQUIRE(peak < 4.0f);   // bounded by the tanh
        const double hz = pitchOf(y, 60000, 90000, 200.0, 3000.0);
        REQUIRE(std::fabs(cents(hz, 1000.0)) < 60.0);
    }
}

TEST_CASE("LadderFilter: stable for extreme inputs and coefficients", "[dsp][va]") {
    dsp::LadderFilter f;
    for (float fc : {5.0f, 1000.0f, 21000.0f})
        for (float r : {0.0f, 0.5f, 1.0f})
            for (int resp = 0; resp < 4; ++resp) {
                f.reset();
                const float g = dsp::LadderFilter::coefG(fc, 48000.0f);
                for (int i = 0; i < 4800; ++i) {
                    const float x = (i % 50 < 25) ? 10.0f : -10.0f;
                    const float v = f.process(x, g, dsp::LadderFilter::feedbackFor(r, dsp::LadderFilter::Model::Transistor),
                                              dsp::LadderFilter::Model::Transistor,
                                              static_cast<dsp::LadderFilter::Response>(resp));
                    REQUIRE(std::isfinite(v));
                    REQUIRE(std::fabs(v) < 200.0f);
                }
            }
}

TEST_CASE("AnalogEnv: exponential stages, timing, retrigger from current level", "[dsp][va]") {
    dsp::AnalogEnv e;
    const auto c = dsp::AnalogEnv::makeCoefs({0.01f, 0.1f, 0.5f, 0.2f}, 48000.0);
    e.noteOn();
    int n = 0;
    while (e.stage() == dsp::AnalogEnv::Stage::Attack && n < 48000) {
        e.next(c);
        ++n;
    }
    REQUIRE(std::abs(n - 480) < 10); // attack completes in ~10 ms
    for (int i = 0; i < 24000; ++i) e.next(c);
    REQUIRE(e.stage() == dsp::AnalogEnv::Stage::Sustain);
    REQUIRE(std::fabs(e.level() - 0.5f) < 1e-3f);
    e.noteOff();
    n = 0;
    float half = -1.0f;
    while (e.isActive() && n < 48000) {
        e.next(c);
        if (++n == 4800) half = e.level();
    }
    // From 0.5 the release ends at tau * ln((0.5 + eps) / eps) ~ 0.88 * 0.2 s.
    REQUIRE(n > static_cast<int>(0.8 * 0.2 * 48000));
    REQUIRE(n <= static_cast<int>(0.2 * 48000));
    REQUIRE(half > 0.0f);
    REQUIRE(half < 0.06f); // exponential: at half the release time a linear ramp would still be at 0.25
    e.noteOn();
    for (int i = 0; i < 100; ++i) e.next(c);
    const float lv = e.level();
    e.noteOn(); // retrigger mid-attack: no reset to 0
    REQUIRE(e.next(c) >= lv);
}

TEST_CASE("Lfo shapes are bounded and S&H holds per cycle", "[dsp][va]") {
    for (int w = 0; w < 6; ++w)
        for (int i = 0; i < 1000; ++i) {
            const float v = dsp::lfoShape(static_cast<dsp::LfoWave>(w), i * 0.0137, 7u);
            REQUIRE(v >= -1.0f);
            REQUIRE(v <= 1.0f);
        }
    REQUIRE(dsp::lfoShape(dsp::LfoWave::SampleHold, 3.1, 1u) == dsp::lfoShape(dsp::LfoWave::SampleHold, 3.9, 1u));
    REQUIRE(dsp::lfoShape(dsp::LfoWave::SampleHold, 3.1, 1u) != dsp::lfoShape(dsp::LfoWave::SampleHold, 4.1, 1u));
}

// ---------------------------------------------------------------------------------------- module

TEST_CASE("VA: module info is consistent", "[va]") {
    const ModuleInfo& info = VaSynth::moduleInfo();
    REQUIRE(info.typeId == "va");
    REQUIRE(static_cast<int>(info.params.size()) == VaSynth::Count);
    for (size_t i = 0; i < info.params.size(); ++i)
        for (size_t j = i + 1; j < info.params.size(); ++j) REQUIRE(info.params[i].id != info.params[j].id);
    // Spot-check enum order vs ids.
    REQUIRE(info.params[VaSynth::Osc2Wave].id == "osc2_wave");
    REQUIRE(info.params[VaSynth::Osc3Detune].id == "osc3_detune");
    REQUIRE(info.params[VaSynth::Cutoff].id == "cutoff");
    REQUIRE(info.params[VaSynth::Lfo2KeySync].id == "lfo2_key_sync");
    REQUIRE(info.params[VaSynth::Mod6Amt].id == "mod6_amt");
    REQUIRE(info.params[VaSynth::VolumeDb].id == "volume_db");
}

TEST_CASE("VA: pitch accuracy for every oscillator waveform", "[va]") {
    rt::resetViolations();
    const char* names[] = {"saw", "pulse", "triangle", "sine", "supersaw"};
    for (int w = 0; w < 5; ++w)
        for (int note : {45, 69, 81}) {
            Rig r;
            cleanOsc(r);
            r.set("osc1_wave", static_cast<float>(w));
            r.set("osc1_detune", 0.0f);
            r.set("osc1_pw", w == 4 ? 0.05f : 0.5f); // supersaw: mix = centre only-ish
            r.run(0.6, {MidiEvent::noteOn(note, 100)});
            const double ref = 440.0 * std::exp2((note - 69) / 12.0);
            const double hz = pitchOf(r.left, at(0.15), at(0.55));
            INFO(names[w] << " note " << note << " -> " << hz << " Hz");
            REQUIRE(std::fabs(cents(hz, ref)) < 3.0);
        }
    // osc2 / osc3 with octave + semitone + fine offsets.
    for (int o = 2; o <= 3; ++o) {
        Rig r;
        cleanOsc(r);
        const std::string p = "osc" + std::to_string(o) + "_";
        r.set("osc1_level", 0.0f);
        r.set((p + "level").c_str(), 1.0f);
        r.set((p + "octave").c_str(), -1.0f);
        r.set((p + "semi").c_str(), 7.0f);
        r.set((p + "fine").c_str(), 50.0f);
        r.run(0.6, {MidiEvent::noteOn(69, 100)});
        const double ref = 440.0 * std::exp2((-12.0 + 7.5) / 12.0);
        const double hz = pitchOf(r.left, at(0.15), at(0.55));
        INFO("osc" << o << " -> " << hz);
        REQUIRE(std::fabs(cents(hz, ref)) < 3.0);
    }
    REQUIRE(rt::violationCount() == 0);
}

TEST_CASE("VA: sub oscillator one and two octaves down", "[va]") {
    for (int so = 0; so < 2; ++so) {
        Rig r;
        cleanOsc(r);
        r.set("osc1_level", 0.0f);
        r.set("sub_level", 1.0f);
        r.set("sub_octave", static_cast<float>(so));
        r.run(0.6, {MidiEvent::noteOn(69, 100)});
        const double ref = so == 0 ? 220.0 : 110.0;
        REQUIRE(std::fabs(cents(pitchOf(r.left, at(0.15), at(0.55)), ref)) < 3.0);
    }
}

TEST_CASE("VA: no aliasing blow-up at high notes", "[va]") {
    struct Case {
        const char* name;
        int wave;
        bool sync;
        double maxDb;
    };
    // Reference: a naive (non-band-limited) saw at C7 on the same metric.
    const double f0 = 440.0 * std::exp2((96 - 69) / 12.0); // C7
    std::vector<float> naive(static_cast<size_t>(kSr));
    for (size_t i = 0; i < naive.size(); ++i) {
        const double ph = std::fmod(static_cast<double>(i) * f0 / kSr, 1.0);
        naive[i] = static_cast<float>(2.0 * ph - 1.0);
    }
    const double naiveDb = inharmonicDb(naive, 0, f0);
    std::printf("[va] alias naive saw: %.1f dB\n", naiveDb);
    const Case cases[] = {{"saw", 0, false, -30.0}, {"pulse", 1, false, -30.0}, {"triangle", 2, false, -45.0},
                          {"sync saw", 0, true, -24.0}};
    for (const auto& c : cases) {
        Rig r;
        cleanOsc(r);
        r.set("osc1_wave", static_cast<float>(c.wave));
        r.set("osc1_sync", c.sync ? 1.0f : 0.0f);
        r.set("osc1_semi", c.sync ? 7.0f : 0.0f);
        r.run(1.0, {MidiEvent::noteOn(96, 100)});
        const double db = inharmonicDb(r.left, at(0.2), f0);
        INFO(c.name << ": inharmonic " << db << " dB");
        std::printf("[va] alias %s: %.1f dB\n", c.name, db);
        REQUIRE(db < c.maxDb);
        REQUIRE(db < naiveDb - 10.0);
    }
    // Extreme top notes stay finite and bounded for every wave, incl. supersaw + FM + sync.
    for (int w = 0; w < 6; ++w) {
        Rig r;
        r.set("osc1_wave", static_cast<float>(w));
        r.set("osc1_sync", 1.0f);
        r.set("osc1_fm", 1.0f);
        r.set("osc3_level", 1.0f);
        r.set("osc1_octave", 3.0f);
        r.set("cutoff", 20000.0f);
        r.run(0.3, {MidiEvent::noteOn(127, 127)});
        const Stats s = stats(r.left);
        REQUIRE(s.finite);
        REQUIRE(s.peak < 4.0f);
    }
}

TEST_CASE("VA: filter self-oscillation stable inside the module", "[va]") {
    for (int model = 0; model < 2; ++model) {
        Rig r;
        r.set("filter_model", static_cast<float>(model));
        r.set("osc1_level", 0.0f);
        r.set("noise_level", 0.02f); // tiny excitation
        r.set("resonance", 1.0f);
        r.set("cutoff", 1000.0f);
        r.set("key_track", 0.0f);
        r.set("drift", 0.0f);
        r.run(2.0, {MidiEvent::noteOn(60, 100)});
        const Stats s = stats(r.left, at(1.0), at(2.0));
        INFO("model " << model << " rms " << s.rms << " peak " << s.peak);
        REQUIRE(s.finite);
        REQUIRE(s.rms > 0.01);
        REQUIRE(s.peak < 2.0f);
        const double hz = pitchOf(r.left, at(1.2), at(1.8), 200.0, 3000.0);
        REQUIRE(std::fabs(cents(hz, 1000.0)) < 100.0);
    }
}

TEST_CASE("VA: every param at min/max and random patches stay finite", "[va]") {
    rt::resetViolations();
    const auto& specs = VaSynth::moduleInfo().params;
    const std::vector<MidiEvent> chord = {MidiEvent::noteOn(36, 127), MidiEvent::noteOn(60, 90),
                                          MidiEvent::noteOn(64, 60, 1, 100), MidiEvent::noteOn(96, 127, 1, 200),
                                          MidiEvent::cc(1, 127, 1, 300), MidiEvent::pitchBend(1.0f, 1, 400)};
    Rig r;
    for (size_t i = 0; i < specs.size(); ++i) {
        for (float v : {specs[i].min, specs[i].max}) {
            r.synth.params().resetToDefaults();
            r.synth.params().set(static_cast<int>(i), v);
            r.synth.reset();
            r.clear();
            r.run(0.15, chord);
            const Stats s = stats(r.left);
            INFO(specs[i].id << " = " << v << " peak " << s.peak);
            REQUIRE(s.finite);
            REQUIRE(s.peak < 8.0f);
        }
    }
    // Random patches (deterministic), each with a held chord, mod wheel, bend and aftertouch.
    dsp::WhiteNoise rng(4242u);
    for (int t = 0; t < 60; ++t) {
        for (size_t i = 0; i < specs.size(); ++i) {
            const auto& sp = specs[i];
            float v = sp.min + (sp.max - sp.min) * rng.nextUnipolar();
            if (sp.id == "volume_db") v = 0.0f;
            if (sp.id == "polyphony" || sp.id == "unison") v = sp.max;
            r.synth.params().set(static_cast<int>(i), v);
        }
        r.synth.reset();
        r.clear();
        MidiEvent atE;
        atE.type = MidiEventType::ChannelPressure;
        atE.valueF = 1.0f;
        atE.sampleOffset = 500;
        auto ev = chord;
        ev.push_back(atE);
        r.run(0.12, ev);
        const Stats s = stats(r.left);
        INFO("random patch " << t << " peak " << s.peak);
        REQUIRE(s.finite);
        REQUIRE(s.peak < 16.0f);
    }
    REQUIRE(rt::violationCount() == 0);
}

TEST_CASE("VA: unison respects the sub-voice budget", "[va]") {
    for (int unison : {1, 3, 8}) {
        Rig r;
        r.set("unison", static_cast<float>(unison));
        r.set("amp_release", 2.0f);
        std::vector<MidiEvent> ev;
        for (int n = 0; n < 16; ++n) ev.push_back(MidiEvent::noteOn(48 + n, 100, 1, static_cast<uint32_t>(n * 10)));
        r.run(0.2, ev);
        const int subs = r.synth.activeSubVoices();
        INFO("unison " << unison << " -> " << subs << " sub-voices");
        REQUIRE(subs <= VaSynth::kSubVoiceBudget);
        REQUIRE(subs == std::min(16, VaSynth::kSubVoiceBudget / unison) * unison);
        REQUIRE(r.synth.activeVoices() == subs);
    }
}

TEST_CASE("VA: legato mode glides without retriggering; mono retriggers", "[va]") {
    for (int mode : {1, 2}) {
        Rig r;
        r.set("voice_mode", static_cast<float>(mode));
        r.set("glide", 0.1f);
        r.run(0.3, {MidiEvent::noteOn(60, 100)});
        auto& v = r.synth.allocator().voice(0);
        REQUIRE(v.note == 60.0f);
        r.run(0.03, {MidiEvent::noteOn(72, 100)});
        INFO("mode " << mode << " note " << v.note);
        REQUIRE(v.note > 60.5f); // gliding: constant time 0.1 s, ~30% done
        REQUIRE(v.note < 70.0f);
        if (mode == 2) REQUIRE(v.timeSinceOn > 0.25f); // legato: no retrigger
        else REQUIRE(v.timeSinceOn < 0.1f);             // mono: envelopes retriggered
        r.run(0.1);
        REQUIRE(v.note == 72.0f);
        // Release 72 -> back to held 60 (glide again).
        r.run(0.03, {MidiEvent::noteOff(72)});
        REQUIRE(v.targetNote == 60.0f);
        REQUIRE(v.note > 61.0f);
    }
}

TEST_CASE("VA: legato-only glide skips detached notes", "[va]") {
    Rig r;
    r.set("voice_mode", 1.0f); // mono retrigger
    r.set("glide", 0.2f);
    r.set("glide_mode", 1.0f);
    r.set("amp_release", 0.5f);
    r.run(0.2, {MidiEvent::noteOn(60, 100), MidiEvent::noteOff(60, 0, 1, at(0.1))});
    auto& v = r.synth.allocator().voice(0);
    REQUIRE(v.isActive()); // still releasing
    r.run(0.02, {MidiEvent::noteOn(67, 100)});
    REQUIRE(v.note == 67.0f); // detached: no glide
    r.run(0.05, {MidiEvent::noteOn(72, 100)});
    REQUIRE(v.note < 71.0f);  // fingered legato: glides
}

TEST_CASE("VA: sustain pedal holds and releases notes", "[va]") {
    Rig r;
    r.set("amp_release", 0.1f);
    r.run(0.2, {MidiEvent::noteOn(60, 100), MidiEvent::cc(64, 127, 1, 100), MidiEvent::noteOff(60, 0, 1, 2000)});
    r.run(0.5);
    REQUIRE(r.synth.activeSubVoices() == 1);
    REQUIRE(stats(r.left, r.left.size() - 4800).rms > 1e-3);
    r.run(0.3, {MidiEvent::cc(64, 0)});
    REQUIRE(r.synth.activeSubVoices() == 0);
}

TEST_CASE("VA: tempo-synced LFO follows the transport", "[va]") {
    // LFO1 square at 1/4 notes, 120 BPM -> 2 Hz tremolo; measure envelope period on a steady tone.
    Rig r;
    cleanOsc(r);
    r.set("osc1_wave", 3.0f); // sine
    r.set("lfo1_wave", 4.0f);
    r.set("lfo1_sync", 1.0f);
    r.set("lfo1_division", 8.0f);
    r.set("lfo1_amp", 1.0f);
    r.run(2.0, {MidiEvent::noteOn(69, 100)});
    int edges = 0;
    bool loud = false;
    for (size_t i = at(0.1); i + 480 < r.left.size(); i += 480) {
        const bool l = stats(r.left, i, i + 480).rms > 0.02;
        if (l && !loud) ++edges;
        loud = l;
    }
    REQUIRE(edges >= 3);
    REQUIRE(edges <= 5); // ~2 Hz over 1.9 s
}
