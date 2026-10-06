// `epiano` instrument and `tremolo` effect tests (ARCHITECTURE §13).

#include "core/ChannelState.h"
#include "core/RtCheck.h"
#include "dsp/BeamModes.h"
#include "dsp/Denormals.h"
#include "dsp/Math.h"
#include "effects/tremolo/TremoloFx.h"
#include "instruments/epiano/EPiano.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

using namespace ks;

namespace {

constexpr double kSr = 48000.0;

struct TimedMidi {
    double t;
    MidiEvent e;
};

// Drives an EPiano directly (no graph), block size 64.
struct Rig {
    EPiano ep;
    ChannelState cs;
    double sr;
    int block;
    std::vector<float> L, R;
    int maxActive = 0;

    explicit Rig(int model, double sampleRate = kSr, int blockSize = 64) : sr(sampleRate), block(blockSize) {
        set("model", static_cast<float>(model));
        ep.prepare(sr, block);
    }
    void set(const char* id, float v) { REQUIRE(ep.params().set(id, v)); }

    // Renders `seconds`, events at absolute times (seconds from the start of this call).
    void render(double seconds, std::vector<TimedMidi> ev = {}) {
        const dsp::ScopedNoDenormals noDenormals; // as in Engine::process (ARCHITECTURE 4.5)
        std::stable_sort(ev.begin(), ev.end(), [](const TimedMidi& a, const TimedMidi& b) { return a.t < b.t; });
        const auto total = static_cast<int64_t>(seconds * sr);
        std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
        std::vector<MidiEvent> blockEv;
        size_t next = 0;
        for (int64_t pos = 0; pos < total; pos += block) {
            const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
            blockEv.clear();
            while (next < ev.size() && static_cast<int64_t>(ev[next].t * sr) < pos + n) {
                MidiEvent e = ev[next].e;
                e.sampleOffset = static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(ev[next].t * sr) - pos));
                cs.apply(e);
                blockEv.push_back(e);
                ++next;
            }
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            AudioBlock b{l.data(), r.data(), n};
            ProcessContext ctx;
            ctx.sampleRate = sr;
            ctx.numSamples = n;
            ctx.channel = &cs;
            ep.process(b, MidiEventSpan(blockEv.data(), blockEv.size()), ctx);
            L.insert(L.end(), l.begin(), l.begin() + n);
            R.insert(R.end(), r.begin(), r.begin() + n);
            maxActive = std::max(maxActive, ep.activeVoices());
        }
    }
    size_t at(double t) const { return std::min(L.size(), static_cast<size_t>(t * sr)); }
};

TimedMidi on(double t, int note, int vel) { return {t, MidiEvent::noteOn(note, vel)}; }
TimedMidi off(double t, int note) { return {t, MidiEvent::noteOff(note)}; }
TimedMidi pedal(double t, int v) { return {t, MidiEvent::cc(64, v)}; }

double rms(const std::vector<float>& x, size_t a, size_t b) {
    double s = 0.0;
    b = std::min(b, x.size());
    if (b <= a) return 0.0;
    for (size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(b - a));
}
double db(double v) { return v > 1e-12 ? 20.0 * std::log10(v) : -240.0; }

// Second-moment spectral centroid sqrt(sum f^2 P / sum P) via the first difference (no FFT needed).
double rmsFrequency(const std::vector<float>& x, size_t a, size_t b, double sr) {
    double e = 0.0, d = 0.0;
    for (size_t i = a + 1; i < b; ++i) {
        e += static_cast<double>(x[i]) * x[i];
        const double df = static_cast<double>(x[i]) - x[i - 1];
        d += df * df;
    }
    const double ratio = std::min(4.0, d / std::max(e, 1e-30));
    return sr / (2.0 * std::numbers::pi) * 2.0 * std::asin(std::sqrt(ratio) * 0.5);
}

// Hann-windowed DFT magnitude at frequency f.
double dftMag(const std::vector<float>& x, size_t a, size_t b, double sr, double f) {
    const double w = 2.0 * std::numbers::pi * f / sr;
    const double wh = 2.0 * std::numbers::pi / static_cast<double>(b - a);
    const double cw = std::cos(w), sw = std::sin(w), ch = std::cos(wh), sh = std::sin(wh);
    double re = 0.0, im = 0.0, pr = 1.0, pi = 0.0, hr = 1.0, hi = 0.0; // rotating phasors (signal, window)
    for (size_t i = a; i < b; ++i) {
        const double v = x[i] * (0.5 - 0.5 * hr);
        re += v * pr;
        im -= v * pi;
        const double npr = pr * cw - pi * sw;
        pi = pi * cw + pr * sw;
        pr = npr;
        const double nhr = hr * ch - hi * sh;
        hi = hi * ch + hr * sh;
        hr = nhr;
    }
    return std::hypot(re, im);
}

// Spectral peak near `guess` (within +-40 cents), refined to 0.05 cent.
double peakFrequency(const std::vector<float>& x, size_t a, size_t b, double sr, double guess) {
    double best = guess, bestMag = -1.0;
    for (double c = -40.0; c <= 40.0; c += 1.0) {
        const double f = guess * std::exp2(c / 1200.0);
        const double m = dftMag(x, a, b, sr, f);
        if (m > bestMag) {
            bestMag = m;
            best = f;
        }
    }
    const double centre = best;
    for (double c = -1.0; c <= 1.0; c += 0.05) {
        const double f = centre * std::exp2(c / 1200.0);
        const double m = dftMag(x, a, b, sr, f);
        if (m > bestMag) {
            bestMag = m;
            best = f;
        }
    }
    return best;
}

double expectedHz(int note, float keyVar) {
    return dsp::noteToHz(static_cast<float>(note) + EPiano::tuningCents(note, keyVar) * 0.01f);
}

// Time (s) after `from` until the 20 ms RMS falls `dropDb` below the level at `from`.
double timeToDrop(const std::vector<float>& x, double sr, double from, double dropDb) {
    const size_t w = static_cast<size_t>(0.02 * sr);
    const size_t a = static_cast<size_t>(from * sr);
    const double ref = db(rms(x, a, a + w));
    for (size_t i = a; i + w < x.size(); i += w)
        if (db(rms(x, i, i + w)) < ref - dropDb) return static_cast<double>(i - a) / sr;
    return 1e9;
}

} // namespace

TEST_CASE("BeamModes: clamped-free ratios and tip shape", "[epiano][dsp]") {
    REQUIRE(std::fabs(dsp::beam::ratio(1) - 6.267) < 0.01);
    REQUIRE(std::fabs(dsp::beam::ratio(2) - 17.547) < 0.01);
    for (int k = 0; k < dsp::beam::kModes; ++k) REQUIRE(std::fabs(std::fabs(dsp::beam::shape(k, 1.0)) - 2.0) < 1e-3);
    REQUIRE(std::fabs(dsp::beam::shape(0, 0.0)) < 1e-12);
}

TEST_CASE("epiano: pitch accuracy across the range (incl. modelled stretch)", "[epiano]") {
    // Deliberate tuning offsets stay small.
    for (int n = 21; n <= 108; ++n) REQUIRE(std::fabs(EPiano::tuningCents(n, 1.0f)) < 6.0f);
    struct Case {
        int model;
        std::vector<int> notes;
    };
    const Case cases[] = {
        {0, {28, 33, 40, 48, 55, 60, 64, 69, 76, 81, 88, 96, 100}},
        {1, {36, 60, 84}},
        {3, {36, 48, 60, 72, 84, 96}},
        {4, {28, 35, 43, 52, 59}},
    };
    for (const auto& c : cases) {
        for (int note : c.notes) {
            Rig rig(c.model);
            const float kv = 0.5f;
            rig.set("key_variation", kv);
            rig.render(1.6, {on(0.0, note, 90)});
            const double hz = expectedHz(note, kv);
            // Window long enough to resolve the (slightly split) fundamental normal modes.
            const double winStart = 0.25, winEnd = note < 45 ? 1.6 : 1.05;
            const double f = peakFrequency(rig.L, rig.at(winStart), rig.at(winEnd), kSr, hz);
            const double cents = 1200.0 * std::log2(f / hz);
            CAPTURE(c.model, note, hz, f, cents);
            REQUIRE(std::fabs(cents) <= 3.0);
        }
    }
}

TEST_CASE("epiano: louder and brighter with velocity", "[epiano]") {
    for (int model : {0, 1, 2, 3, 4}) {
        for (int note : {model == 4 ? 33 : 48, model == 4 ? 50 : 67}) {
            double lastRms = -1.0, lastBright = -1.0;
            for (int vel : {15, 35, 55, 75, 95, 115, 127}) {
                Rig rig(model);
                rig.set("noise", 0.0f); // brightness from the strike, not the click
                rig.render(0.6, {on(0.0, note, vel)});
                const double r = rms(rig.L, 0, rig.at(0.5));
                const double b = rmsFrequency(rig.L, 0, rig.at(0.15), kSr);
                CAPTURE(model, note, vel, db(r), b);
                REQUIRE(r > lastRms * 1.05);     // strictly louder
                REQUIRE(b > lastBright * 1.005); // strictly brighter
                lastRms = r;
                lastBright = b;
            }
        }
    }
}

TEST_CASE("epiano: decay is longer in the bass than in the treble", "[epiano]") {
    for (int model : {0, 3}) {
        double last = 1e9;
        for (int note : {36, 60, 84}) {
            Rig rig(model);
            rig.set("noise", 0.0f);
            rig.render(8.0, {on(0.0, note, 100)});
            const double t = timeToDrop(rig.L, kSr, 0.1, 30.0);
            CAPTURE(model, note, t);
            REQUIRE(t < 1e8); // did decay at all
            REQUIRE(t < last);
            last = t;
        }
    }
}

TEST_CASE("epiano: key release damps quickly, top keys ring longer", "[epiano]") {
    for (int model : {0, 1, 3, 4}) {
        Rig rig(model);
        const int note = model == 4 ? 48 : 60;
        rig.render(2.0, {on(0.0, note, 100), off(1.0, note)});
        const double before = db(rms(rig.L, rig.at(0.97), rig.at(0.99)));
        const double after = db(rms(rig.L, rig.at(1.15), rig.at(1.17)));
        CAPTURE(model, before, after);
        REQUIRE(after < before - 40.0); // >= 40 dB down within 150 ms
        REQUIRE(rig.ep.activeVoices() == 0);
    }
    // Rhodes top keys (> G#6) have no effective dampers: released they ring longer than below the break.
    auto releaseTime = [](int note) {
        Rig rig(0);
        rig.set("noise", 0.0f);
        rig.render(3.0, {on(0.0, note, 100), off(0.5, note)});
        return timeToDrop(rig.L, kSr, 0.48, 40.0) - 0.02;
    };
    const double below = releaseTime(92), above = releaseTime(98);
    CAPTURE(below, above);
    REQUIRE(above > 2.0 * below);
}

TEST_CASE("epiano: pedal churn on a long note stays stable (no pole drift)", "[epiano]") {
    Rig rig(4, 96000.0, 32);
    rig.set("decay", 4.0f);
    rig.set("noise", 0.0f);
    // Key released under the pedal; the pedal flutters between fully and almost fully lifted, so the felt
    // engagement (and every mode radius) is updated thousands of times.
    std::vector<TimedMidi> ev = {pedal(0.0, 127), on(0.0, 28, 110), off(0.3, 28)};
    for (int i = 0; i < 1500; ++i) ev.push_back(pedal(0.5 + 0.01 * i, i % 2 ? 80 : 127));
    rig.render(16.0, ev);
    double prev = 1e9;
    for (double t = 0.5; t < 15.5; t += 1.0) {
        const double l = rms(rig.L, rig.at(t), rig.at(t + 0.5));
        REQUIRE(std::isfinite(l));
        REQUIRE(l <= prev * 1.01); // never grows
        prev = l;
    }
}

TEST_CASE("epiano: sustain pedal holds, half pedal partially damps, pedal up damps", "[epiano]") {
    auto levelAt = [](int pedalValue) {
        Rig rig(0);
        rig.set("noise", 0.0f);
        std::vector<TimedMidi> ev = {on(0.0, 60, 100), off(0.3, 60)};
        if (pedalValue >= 0) ev.insert(ev.begin(), pedal(0.0, pedalValue));
        rig.render(1.6, ev);
        return db(rms(rig.L, rig.at(1.0), rig.at(1.1)));
    };
    Rig held(0);
    held.set("noise", 0.0f);
    held.render(1.6, {on(0.0, 60, 100)});
    const double heldDb = db(rms(held.L, held.at(1.0), held.at(1.1)));
    const double full = levelAt(127), half = levelAt(60), none = levelAt(-1);
    CAPTURE(heldDb, full, half, none);
    REQUIRE(std::fabs(full - heldDb) < 1.0); // pedal down = dampers lifted, rings like a held key
    REQUIRE(half < full - 6.0);              // half pedal: felt partially touching
    REQUIRE(half > none + 20.0);
    REQUIRE(none < heldDb - 60.0);

    // Pedal up damps the sustained note.
    Rig rig(0);
    rig.render(2.0, {pedal(0.0, 127), on(0.05, 60, 100), off(0.2, 60), pedal(1.0, 0)});
    REQUIRE(db(rms(rig.L, rig.at(1.2), rig.at(1.25))) < db(rms(rig.L, rig.at(0.95), rig.at(1.0))) - 40.0);
    REQUIRE(rig.ep.activeVoices() == 0);
}

TEST_CASE("epiano: re-strike under pedal reuses the voice", "[epiano]") {
    Rig rig(0);
    std::vector<TimedMidi> ev = {pedal(0.0, 127)};
    for (int i = 0; i < 6; ++i) {
        ev.push_back(on(0.05 + 0.2 * i, 64, 90));
        ev.push_back(off(0.15 + 0.2 * i, 64));
    }
    rig.render(1.5, ev);
    REQUIRE(rig.maxActive == 1);
}

TEST_CASE("epiano: 32-voice polyphony, smooth stealing", "[epiano]") {
    Rig rig(0);
    std::vector<TimedMidi> ev;
    for (int i = 0; i < 48; ++i) ev.push_back(on(0.01 * i, 36 + i, 100));
    rig.render(1.0, ev);
    REQUIRE(rig.maxActive <= EPiano::kMaxVoices + EPiano::kGhosts);
    for (float x : rig.L) REQUIRE(std::isfinite(x));

    // Stealing fades the old note out (6 ms ghost) instead of cutting it: compare with a no-steal render.
    auto renderPoly = [](int poly) {
        Rig r(0);
        r.set("polyphony", static_cast<float>(poly));
        r.set("noise", 0.0f);
        r.render(1.0, {on(0.0, 48, 120), on(0.5, 72, 1)});
        return r.L;
    };
    const auto steal = renderPoly(1), noSteal = renderPoly(2);
    float peakOld = 0.0f, maxStep = 0.0f;
    for (size_t i = static_cast<size_t>(0.4 * kSr); i < static_cast<size_t>(0.5 * kSr); ++i)
        peakOld = std::max(peakOld, std::fabs(noSteal[i]));
    for (size_t i = static_cast<size_t>(0.5 * kSr); i < static_cast<size_t>(0.53 * kSr); ++i) {
        const float d0 = noSteal[i - 1] - steal[i - 1], d1 = noSteal[i] - steal[i];
        maxStep = std::max(maxStep, std::fabs(d1 - d0));
    }
    CAPTURE(peakOld, maxStep);
    REQUIRE(peakOld > 0.01f);
    REQUIRE(maxStep < 0.1f * peakOld); // the removed (stolen) signal has no jump
}

TEST_CASE("epiano: no NaN/Inf at parameter and key extremes", "[epiano]") {
    rt::resetViolations();
    const ModuleInfo& info = EPiano::moduleInfo();
    for (int model = 0; model < 5; ++model) {
        for (int corner = 0; corner < 3; ++corner) {
            Rig rig(model, corner == 2 ? 96000.0 : 44100.0, corner == 1 ? 512 : 32);
            for (const ParamSpec& p : info.params) {
                if (p.id == "model" || p.id == "polyphony") continue;
                const float v = corner == 0 ? p.min : p.max;
                rig.set(p.id.c_str(), v);
            }
            std::vector<TimedMidi> ev = {{0.0, MidiEvent::pitchBend(corner == 0 ? -1.0f : 1.0f)}, pedal(0.0, 64)};
            for (int n : {0, 1, 21, 28, 60, 100, 108, 120, 127}) {
                ev.push_back(on(0.0, n, 127));
                ev.push_back(on(0.3, n, 1));
                ev.push_back(off(0.6, n));
            }
            ev.push_back({0.7, MidiEvent::allNotesOff()});
            rig.render(1.2, ev);
            float peak = 0.0f;
            for (size_t i = 0; i < rig.L.size(); ++i) {
                REQUIRE(std::isfinite(rig.L[i]));
                REQUIRE(std::isfinite(rig.R[i]));
                peak = std::max({peak, std::fabs(rig.L[i]), std::fabs(rig.R[i])});
            }
            CAPTURE(model, corner, peak);
            REQUIRE(peak < 8.0f);
        }
    }
    if (rt::checksEnabled()) REQUIRE(rt::violationCount() == 0);
}

TEST_CASE("epiano: Suitcase stereo vibrato pans, other models are mono", "[epiano]") {
    for (int model : {0, 2}) {
        Rig rig(model);
        rig.set("stereo_depth", 0.8f);
        rig.set("stereo_rate", 4.0f);
        rig.render(1.5, {on(0.0, 60, 100)});
        double maxDiff = 0.0;
        for (size_t i = 0; i < rig.L.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(double(rig.L[i]) - rig.R[i]));
        if (model == 2) REQUIRE(maxDiff > 0.01);
        else REQUIRE(maxDiff == 0.0);
    }
}

TEST_CASE("epiano: 32-voice CPU benchmark", "[.bench][epiano]") {
    // ks-tests "[.bench]": 32 held notes under the pedal (re-struck every second), 10 s at 48 kHz / 64.
    for (int model : {0, 3, 4}) {
        Rig rig(model);
        std::vector<TimedMidi> ev = {pedal(0.0, 127)};
        for (int s = 0; s < 10; ++s)
            for (int i = 0; i < 32; ++i) ev.push_back(on(s + 0.001 * i, 30 + 2 * i, 60 + (i * 7) % 67));
        const auto t0 = std::chrono::steady_clock::now();
        rig.render(10.0, ev);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        WARN("model " << model << ": " << rig.maxActive << " voices, realtime factor " << 10.0 / secs << "x");
        REQUIRE(rig.maxActive >= 32);
        REQUIRE(10.0 / secs > 2.0);
    }
}

// --------------------------------------------------------------------------------------------------------------
// tremolo

namespace {
struct TremRig {
    TremoloFx fx;
    ChannelState cs;
    ProcessContext ctx;
    std::vector<float> L, R;
    TremRig() {
        fx.prepare(kSr, 64);
        ctx.sampleRate = kSr;
        ctx.channel = &cs;
    }
    void set(const char* id, float v) { REQUIRE(fx.params().set(id, v)); }
    // Feeds a constant 1.0 on both channels (output = gain curve).
    void run(double seconds) {
        fx.reset();
        L.clear();
        R.clear();
        std::vector<float> l(64), r(64);
        const int total = static_cast<int>(seconds * kSr);
        for (int pos = 0; pos < total; pos += 64) {
            std::fill(l.begin(), l.end(), 1.0f);
            std::fill(r.begin(), r.end(), 1.0f);
            AudioBlock b{l.data(), r.data(), 64};
            ctx.numSamples = 64;
            fx.process(b, {}, ctx);
            ctx.transport.ppqPosition += 64.0 / kSr * ctx.transport.tempo / 60.0;
            L.insert(L.end(), l.begin(), l.end());
            R.insert(R.end(), r.begin(), r.end());
        }
    }
};

// Cycles per second of a gain curve: upward crossings of its mid level.
double measuredRate(const std::vector<float>& x) {
    const auto [mn, mx] = std::minmax_element(x.begin() + static_cast<long>(0.1 * kSr), x.end());
    const float mid = 0.5f * (*mn + *mx);
    int cross = 0;
    size_t first = 0, last = 0;
    for (size_t i = static_cast<size_t>(0.1 * kSr); i < x.size(); ++i) {
        if (x[i - 1] < mid && x[i] >= mid) {
            if (cross == 0) first = i;
            last = i;
            ++cross;
        }
    }
    return cross > 1 ? (cross - 1) / (static_cast<double>(last - first) / kSr) : 0.0;
}
} // namespace

TEST_CASE("tremolo: amplitude depth and rate", "[tremolo]") {
    for (int shape : {0, 1, 2}) {
        TremRig t;
        t.set("shape", static_cast<float>(shape));
        t.set("depth", 0.6f);
        t.set("rate", 4.0f);
        t.run(3.0);
        const auto [mn, mx] = std::minmax_element(t.L.begin() + static_cast<long>(0.2 * kSr), t.L.end());
        CAPTURE(shape, *mn, *mx);
        REQUIRE(std::fabs(*mx - 1.0f) < 0.01f);
        REQUIRE(std::fabs(*mn - 0.4f) < 0.01f);
        REQUIRE(std::fabs(measuredRate(t.L) - 4.0) < 0.02);
        REQUIRE(t.L == t.R); // stereo phase 0
    }
}

TEST_CASE("tremolo: stereo phase, autopan equal power, tempo sync", "[tremolo]") {
    {
        TremRig t;
        t.set("depth", 1.0f);
        t.set("stereo_phase", 180.0f);
        t.run(1.0);
        for (size_t i = 0; i < t.L.size(); i += 97) REQUIRE(std::fabs(t.L[i] + t.R[i] - 1.0f) < 1e-3f);
    }
    {
        TremRig t;
        t.set("mode", 1.0f);
        t.set("depth", 0.8f);
        t.set("rate", 2.0f);
        t.run(2.0);
        float mn = 10.0f;
        for (size_t i = 0; i < t.L.size(); ++i) {
            REQUIRE(std::fabs(t.L[i] * t.L[i] + t.R[i] * t.R[i] - 2.0f) < 1e-3f);
            mn = std::min(mn, t.L[i] * t.L[i]);
        }
        REQUIRE(std::fabs(mn - 0.2f) < 0.01f); // L power swings to 1 - depth
        REQUIRE(std::fabs(measuredRate(t.L) - 2.0) < 0.02);
    }
    {
        TremRig t;
        t.set("sync", 1.0f);
        t.set("division", 7.0f); // 1/8 at 120 BPM = 4 Hz
        t.ctx.transport.tempo = 120.0;
        t.ctx.transport.playing = true;
        t.run(3.0);
        REQUIRE(std::fabs(measuredRate(t.L) - 4.0) < 0.02);
        REQUIRE(TremoloFx::divisionBeats(4) == 1.0);
    }
    {
        // Phase lock: starting off-grid, the LFO converges to the ppq phase (1/4 = one cycle per beat).
        TremRig t;
        t.set("sync", 1.0f);
        t.set("division", 4.0f);
        t.set("depth", 1.0f);
        t.ctx.transport.tempo = 100.0;
        t.ctx.transport.playing = true;
        t.ctx.transport.ppqPosition = 0.3;
        t.run(2.0);
        for (size_t i = t.L.size() - 2000; i < t.L.size(); i += 250) {
            const double ppq = 0.3 + static_cast<double>(i) / kSr * 100.0 / 60.0;
            const float expect =
                1.0f - 0.5f * (1.0f - TremoloFx::lfoValue(ppq, TremoloFx::ShapeId::Sine, 0.5f));
            REQUIRE(std::fabs(t.L[i] - expect) < 0.01f);
        }
    }
}
