#include "dsp/Adsr.h"
#include "dsp/Limiter.h"
#include "dsp/PolyBlep.h"
#include "dsp/Smoother.h"
#include "dsp/SoftClip.h"
#include "dsp/Svf.h"
#include "transport/Metronome.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace ks;
using namespace ks::dsp;

TEST_CASE("PolyBLEP saw: frequency, range, no DC", "[dsp]") {
    PolyBlepOsc o;
    o.setSampleRate(48000.0);
    o.setFrequency(1000.0f);
    double sum = 0.0;
    float peak = 0.0f;
    int crossings = 0;
    float prev = o.next(Wave::Saw);
    for (int i = 0; i < 48000; ++i) {
        const float x = o.next(Wave::Saw);
        sum += x;
        peak = std::max(peak, std::fabs(x));
        if (prev >= -0.5f && x < -0.5f) ++crossings; // entering the bottom of the ramp: once per cycle // reset edge once per cycle (spread over ~2 samples by the BLEP)
        prev = x;
    }
    REQUIRE(std::abs(crossings - 1000) <= 1);
    REQUIRE(peak < 1.2f);
    REQUIRE(std::fabs(sum / 48000.0) < 0.01);
    for (Wave w : {Wave::Square, Wave::Triangle, Wave::Sine})
        for (int i = 0; i < 4800; ++i) REQUIRE(std::isfinite(o.next(w)));
}

TEST_CASE("ADSR stages and release to idle", "[dsp]") {
    Adsr e;
    e.setSampleRate(48000.0);
    e.setParams({0.01f, 0.05f, 0.5f, 0.1f});
    e.noteOn();
    for (int i = 0; i < 480; ++i) e.next();
    REQUIRE(e.level() > 0.95f);
    for (int i = 0; i < 48000 / 5; ++i) e.next();
    REQUIRE(e.stage() == Adsr::Stage::Sustain);
    REQUIRE(std::fabs(e.level() - 0.5f) < 0.01f);
    e.noteOff();
    int n = 0;
    while (e.isActive() && n < 48000) {
        e.next();
        ++n;
    }
    REQUIRE_FALSE(e.isActive());
    REQUIRE(n <= static_cast<int>(0.1 * 48000) + 10);
}

TEST_CASE("SVF stable across params incl. extreme resonance", "[dsp]") {
    Svf f;
    f.setSampleRate(48000.0);
    for (float cut : {10.0f, 100.0f, 1000.0f, 10000.0f, 30000.0f})
        for (float res : {0.0f, 0.5f, 1.0f}) {
            f.reset();
            f.setCutoff(cut, res);
            float peak = 0.0f;
            for (int i = 0; i < 4800; ++i) {
                const float x = (i % 100) < 50 ? 1.0f : -1.0f;
                const float y = f.process(x, Svf::Mode::LowPass);
                REQUIRE(std::isfinite(y));
                peak = std::max(peak, std::fabs(y));
            }
            REQUIRE(peak < 200.0f);
        }
    // DC passes the low-pass with unity gain.
    f.reset();
    f.setCutoff(1000.0f, 0.0f);
    float y = 0.0f;
    for (int i = 0; i < 48000; ++i) y = f.process(1.0f, Svf::Mode::LowPass);
    REQUIRE(std::fabs(y - 1.0f) < 1e-3f);
}

TEST_CASE("Smoothers converge", "[dsp]") {
    OnePoleSmoother s;
    s.prepare(48000.0, 0.01f);
    s.snap(0.0f);
    s.setTarget(1.0f);
    for (int i = 0; i < 48000; ++i) s.next();
    REQUIRE(s.value() == 1.0f);
    LinearSmoother l;
    l.prepare(48000.0, 0.01f);
    l.setTarget(2.0f);
    for (int i = 0; i < 480; ++i) l.next();
    REQUIRE(l.value() == 2.0f);
    REQUIRE_FALSE(l.isSmoothing());
}

TEST_CASE("Soft clip and safety limiter bound the output", "[dsp]") {
    REQUIRE(softClipKnee(0.5f, 0.8f, 0.98f) == 0.5f);
    REQUIRE(softClipKnee(100.0f, 0.8f, 0.98f) <= 0.98f);
    REQUIRE(softClipKnee(-100.0f, 0.8f, 0.98f) >= -0.98f);
    SafetyLimiter lim;
    lim.setCeilingDb(-0.3f);
    lim.prepare(48000.0);
    std::vector<float> l(4800), r(4800);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = 8.0f * std::sin(0.05f * static_cast<float>(i));
        r[i] = (i == 100) ? std::nanf("") : -l[i];
    }
    lim.process(l.data(), r.data(), static_cast<int>(l.size()));
    for (size_t i = 0; i < l.size(); ++i) {
        REQUIRE(std::isfinite(l[i]));
        REQUIRE(std::isfinite(r[i]));
        REQUIRE(std::fabs(l[i]) < 0.97f);
        REQUIRE(std::fabs(r[i]) < 0.97f);
    }
}

TEST_CASE("Metronome clicks on beats", "[dsp][transport]") {
    Metronome m;
    m.prepare(48000.0);
    m.setEnabled(true);
    m.setVolume(1.0f);
    TransportInfo t;
    t.tempo = 120.0; // beat = 24000 samples
    t.playing = true;
    std::vector<float> l(64), r(64);
    int onsets = 0;
    bool wasSilent = true;
    for (int b = 0; b < 48000 / 64; ++b) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        AudioBlock blk{l.data(), r.data(), 64};
        m.process(blk, t, b == 0, 48000.0);
        float pk = 0.0f;
        for (float x : l) pk = std::max(pk, std::fabs(x));
        if (pk > 1e-3f && wasSilent) ++onsets;
        wasSilent = pk < 1e-4f;
        t.ppqPosition += 64.0 * t.tempo / (60.0 * 48000.0);
    }
    REQUIRE(onsets == 2); // beats 0 and 1 within one second
}

TEST_CASE("Metronome never drops beats at odd block sizes", "[dsp][transport]") {
    for (int block : {37, 64, 113}) {
        Metronome m;
        m.prepare(48000.0);
        m.setEnabled(true);
        TransportInfo t;
        t.tempo = 137.0;
        t.playing = true;
        std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
        int onsets = 0;
        int silentRun = 1000;
        for (int pos = 0; pos < 48000 * 10; pos += block) {
            std::fill(l.begin(), l.end(), 0.0f);
            AudioBlock blk{l.data(), r.data(), block};
            m.process(blk, t, pos == 0, 48000.0);
            for (float x : l) {
                if (std::fabs(x) > 1e-6f) {
                    if (silentRun > 100) ++onsets;
                    silentRun = 0;
                } else {
                    ++silentRun;
                }
            }
            t.ppqPosition += block * t.tempo / (60.0 * 48000.0);
        }
        INFO("block " << block);
        const int expected = static_cast<int>(std::floor(10.0 * 137.0 / 60.0)) + 1;
        REQUIRE(onsets == expected);
    }
}
