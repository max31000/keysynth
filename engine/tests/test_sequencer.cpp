// DrumSequencer + Transport timing, patterns, RhythmNode integration (ARCHITECTURE §8).

#include "core/Engine.h"
#include "core/GraphBuilder.h"
#include "core/PatchModel.h"
#include "core/RtCheck.h"
#include "render/OfflineRenderer.h"
#include "transport/DrumSequencer.h"
#include "transport/Pattern.h"
#include "transport/Transport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace ks;
using nlohmann::json;

namespace {

Pattern allSteps(int num = 4, int den = 4, int spb = 4, int bars = 1, int note = 36) {
    Pattern p;
    p.numerator = num;
    p.denominator = den;
    p.stepsPerBeat = spb;
    p.bars = bars;
    PatternTrack t;
    t.note = note;
    t.steps.assign(static_cast<size_t>(p.numSteps()), 100);
    p.tracks.push_back(t);
    p.normalize();
    return p;
}

struct Onset {
    int64_t sample;
    int note;
    int velocity;
};

// Drives Transport + DrumSequencer like the Engine does; returns absolute onsets.
std::vector<Onset> runSequencer(const Pattern& p, int block, int64_t total, double tempo, float swing = 0.0f,
                                double origin = 0.0, double sr = 48000.0) {
    Transport tr;
    tr.setTempo(tempo);
    tr.setTimeSignature(p.numerator, p.denominator);
    tr.setPlaying(true);
    DrumSequencer seq;
    seq.setPattern(toRt(p));
    std::vector<Onset> out;
    std::vector<MidiEvent> ev(256);
    for (int64_t pos = 0; pos < total; pos += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
        bool started = false;
        const TransportInfo t = tr.beginBlock(started);
        if (started) seq.start(origin);
        const int c = seq.generate(t, n, sr, swing, true, ev.data(), 256);
        for (int i = 0; i < c; ++i) {
            REQUIRE(ev[static_cast<size_t>(i)].sampleOffset < static_cast<uint32_t>(n));
            if (i > 0) REQUIRE(ev[static_cast<size_t>(i)].sampleOffset >= ev[static_cast<size_t>(i - 1)].sampleOffset);
            out.push_back({pos + ev[static_cast<size_t>(i)].sampleOffset, ev[static_cast<size_t>(i)].data1,
                           ev[static_cast<size_t>(i)].value7});
        }
        tr.endBlock(n, sr);
    }
    return out;
}

Pattern loadPatternFile(const std::string& rel) {
    std::ifstream f(std::filesystem::path(KS_SOURCE_DIR) / rel, std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<std::string> w;
    Pattern p = patternFromJson(json::parse(ss.str()), &w);
    INFO(rel);
    for (const auto& s : w) WARN(s);
    REQUIRE(w.empty());
    return p;
}

} // namespace

TEST_CASE("sequencer: step onsets are sample-accurate at 120 BPM for any block size", "[transport][sequencer]") {
    const Pattern p = allSteps();
    for (int block : {32, 64, 333, 1}) {
        // 4 bars at 120 BPM = 8 s; a 16th = 6000 samples at 48 kHz.
        const auto on = runSequencer(p, block, 384000, 120.0);
        INFO("block " << block);
        REQUIRE(on.size() == 64);
        for (size_t k = 0; k < on.size(); ++k) REQUIRE(on[k].sample == static_cast<int64_t>(k) * 6000);
    }
}

TEST_CASE("sequencer: fractional step lengths land on the first sample at/after the exact time", "[transport][sequencer]") {
    const Pattern p = allSteps();
    const double tempo = 137.0, sr = 44100.0;
    const double stepSamples = 60.0 * sr / tempo / 4.0;
    for (int block : {32, 333}) {
        const auto on = runSequencer(p, block, 441000, tempo, 0.0f, 0.0, sr);
        REQUIRE(on.size() == 92); // 10 s / 4828.5 samples
        for (size_t k = 0; k < on.size(); ++k) {
            const double exact = static_cast<double>(k) * stepSamples;
            INFO("block " << block << " step " << k);
            REQUIRE(on[k].sample == static_cast<int64_t>(std::ceil(exact - 1e-4)));
        }
    }
}

TEST_CASE("sequencer: swing delays every second step", "[transport][sequencer]") {
    const Pattern p = allSteps();
    const auto on = runSequencer(p, 64, 96000, 120.0, 0.5f);
    REQUIRE(on.size() == 16);
    for (size_t k = 0; k < on.size(); ++k) {
        const int64_t expected = static_cast<int64_t>(k) * 6000 + ((k & 1) ? 1500 : 0); // 0.5 * half a step
        REQUIRE(on[k].sample == expected);
    }
    // Triplet feel: swing 1/3 puts the off-step at 2/3 of the pair.
    const auto tri = runSequencer(p, 333, 24000, 120.0, 1.0f / 3.0f);
    REQUIRE(tri.size() == 4);
    REQUIRE(tri[1].sample == 6000 + 1000);
}

TEST_CASE("sequencer: 7/4 bar length and wrap", "[transport][sequencer]") {
    Pattern p = allSteps(7, 4, 4, 1);
    REQUIRE(p.numSteps() == 28);
    REQUIRE(p.barPpq() == 7.0);
    // Only step 0 sounds: one onset per 7/4 bar = 7 beats = 168000 samples at 120 BPM.
    std::fill(p.tracks[0].steps.begin(), p.tracks[0].steps.end(), 0);
    p.tracks[0].steps[0] = 100;
    const auto on = runSequencer(p, 64, 168000 * 3 + 10, 120.0);
    REQUIRE(on.size() == 4);
    for (size_t k = 0; k < on.size(); ++k) REQUIRE(on[k].sample == static_cast<int64_t>(k) * 168000);
    // 6/8 with 2 steps per eighth: step = 1/16, 12 steps per bar, bar = 3 quarters.
    Pattern q = allSteps(6, 8, 2, 1);
    REQUIRE(q.numSteps() == 12);
    REQUIRE(q.stepPpq() == 0.25);
    REQUIRE(q.barPpq() == 3.0);
}

TEST_CASE("sequencer: count-in origin delays the first step by one bar", "[transport][sequencer]") {
    const Pattern p = allSteps();
    const auto on = runSequencer(p, 333, 96000 + 6001, 120.0, 0.0f, 4.0);
    REQUIRE(on.size() == 2);
    REQUIRE(on[0].sample == 96000);
    REQUIRE(on[1].sample == 102000);
}

TEST_CASE("sequencer: accent, mute and velocity", "[transport][sequencer]") {
    Pattern p = allSteps();
    p.accentAmount = 0.5f;
    p.accent[0] = 1;
    p.tracks[0].steps[1] = 40;
    PatternTrack muted;
    muted.note = 38;
    muted.mute = true;
    muted.steps.assign(16, 100);
    p.tracks.push_back(muted);
    p.normalize();
    const auto on = runSequencer(p, 64, 12000, 120.0);
    REQUIRE(on.size() == 2); // muted track silent
    REQUIRE(on[0].velocity == 127); // 100 + 32 clamped
    REQUIRE(on[1].velocity == 40);
}

TEST_CASE("pattern: JSON parse (strings, arrays), normalize, round trip", "[transport][preset]") {
    const json j = json::parse(R"({
        "format": 1, "name": "T", "tempo": 90, "time_sig": [3, 4], "steps_per_beat": 4, "bars": 2, "swing": 0.2,
        "kit": "industrial",
        "tracks": [ { "name": "K", "note": 36, "steps": "x... X... o... | 9..1" },
                    { "name": "S", "note": 38, "steps": [0, 0, 0, 0, 120, 300, -5] } ],
        "accent": "x..........."
    })");
    std::vector<std::string> w;
    Pattern p = patternFromJson(j, &w);
    REQUIRE(p.numSteps() == 24);
    REQUIRE(p.kit == 3);
    REQUIRE(p.tracks[0].steps.size() == 24);
    REQUIRE(p.tracks[0].steps[0] == 100);
    REQUIRE(p.tracks[0].steps[4] == 127);
    REQUIRE(p.tracks[0].steps[8] == 50);
    REQUIRE(p.tracks[0].steps[12] == 127);
    REQUIRE(p.tracks[0].steps[15] == 14);
    REQUIRE(p.tracks[1].steps[4] == 120);
    REQUIRE(p.tracks[1].steps[5] == 127);
    REQUIRE(p.tracks[1].steps[6] == 0);
    REQUIRE(p.accent[0] == 1);
    const Pattern q = patternFromJson(patternToJson(p));
    REQUIRE(patternToJson(q) == patternToJson(p));
    // Meter change keeps steps by index.
    p.setTimeSignature(4, 4);
    REQUIRE(p.numSteps() == 32);
    REQUIRE(p.tracks[0].steps[4] == 127);
    // Too long: 4 bars of 16/4 x 8 = 512 -> clamped to <= 256.
    Pattern big = allSteps(16, 4, 8, 4);
    REQUIRE(big.numSteps() <= kPatternMaxSteps);
}

TEST_CASE("pattern: every factory pattern parses cleanly", "[transport][preset]") {
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(KS_SOURCE_DIR) / "presets" / "patterns")) {
        if (e.path().extension() != ".json") continue;
        const Pattern p = loadPatternFile("presets/patterns/" + e.path().filename().string());
        REQUIRE(!p.tracks.empty());
        REQUIRE(p.tempo > 0);
        int hits = 0;
        for (const auto& t : p.tracks)
            for (uint8_t v : t.steps) hits += v > 0;
        REQUIRE(hits > 4);
        ++n;
    }
    REQUIRE(n >= 10);
    REQUIRE(loadPatternFile("presets/patterns/money-7-4.json").numSteps() == 28);
}

namespace {

struct EngineRig {
    PatchModel model{defaultRegistry()};
    Engine engine;
    std::vector<float> l, r;
    explicit EngineRig(int block) {
        engine.prepare(48000.0, block);
        auto b = GraphBuilder::build(model.patch(), defaultRegistry(), nullptr, 48000.0, block);
        engine.publish(std::move(b.graph));
        l.assign(static_cast<size_t>(block), 0.0f);
        r.assign(static_cast<size_t>(block), 0.0f);
    }
    // Returns block peak.
    float block(int n) {
        AudioBlock b{l.data(), r.data(), n};
        engine.processBlock(b);
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = std::max({pk, std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)])});
        return pk;
    }
};

} // namespace

TEST_CASE("engine: rhythm node plays the pattern, stop leaves no stuck voices, RT-safe", "[transport][sequencer][rt]") {
    const uint64_t rt0 = rt::violationCount();
    EngineRig rig(64);
    rig.engine.sequencer().setPattern(toRt(loadPatternFile("presets/patterns/du-hast-stomp.json")));
    rig.engine.transport().setTempo(125.0);
    rig.engine.transport().setPlaying(true);
    float playPeak = 0.0f;
    int maxVoices = 0;
    for (int i = 0; i < 48000 * 3 / 64; ++i) {
        playPeak = std::max(playPeak, rig.block(64));
        maxVoices = std::max(maxVoices, rig.engine.swapper().activeVoices());
    }
    REQUIRE(playPeak > 0.05f);
    REQUIRE(playPeak <= 1.0f);
    REQUIRE(maxVoices > 0);
    REQUIRE(rig.engine.currentStep() >= 0);
    rig.engine.transport().setPlaying(false);
    float tail = 0.0f;
    for (int i = 0; i < 48000 * 4 / 64; ++i) {
        const float pk = rig.block(64);
        if (i > 48000 * 3 / 64) tail = std::max(tail, pk);
    }
    REQUIRE(rig.engine.swapper().activeVoices() == 0);
    REQUIRE(tail < 1e-5f);
    REQUIRE(rig.engine.currentStep() == -1);
    // Restart: plays again from step 0.
    rig.engine.transport().setPlaying(true);
    float again = 0.0f;
    for (int i = 0; i < 48000 / 64; ++i) again = std::max(again, rig.block(64));
    REQUIRE(again > 0.05f);
    REQUIRE(rt::violationCount() == rt0);
}

TEST_CASE("engine: drums off / volume 0 silence the rhythm node", "[transport][sequencer]") {
    EngineRig rig(128);
    rig.engine.sequencer().setPattern(toRt(allSteps()));
    rig.engine.transport().setPlaying(true);
    rig.engine.rhythm().drumsEnabled.store(false);
    float pk = 0.0f;
    for (int i = 0; i < 100; ++i) pk = std::max(pk, rig.block(128));
    REQUIRE(pk < 1e-6f);
    rig.engine.rhythm().drumsEnabled.store(true);
    rig.engine.rhythm().drumsVolume.store(0.0f);
    for (int i = 0; i < 4; ++i) rig.block(128); // ramp to 0
    pk = 0.0f;
    for (int i = 0; i < 100; ++i) pk = std::max(pk, rig.block(128));
    REQUIRE(pk < 1e-6f);
}

TEST_CASE("engine: pattern swaps and graph rebuilds while playing are RT-safe", "[transport][sequencer][rt][swap]") {
    const uint64_t rt0 = rt::violationCount();
    EngineRig rig(333);
    const RtPattern a = toRt(loadPatternFile("presets/patterns/house-four-on-floor.json"));
    const RtPattern b = toRt(loadPatternFile("presets/patterns/money-7-4.json"));
    const RtPattern c = toRt(loadPatternFile("presets/patterns/riders-shuffle.json"));
    rig.engine.sequencer().setPattern(a);
    rig.engine.transport().setPlaying(true);
    float peak = 0.0f;
    for (int i = 0; i < 600; ++i) {
        if (i % 37 == 0) rig.engine.sequencer().setPattern(i % 3 == 0 ? a : i % 3 == 1 ? b : c);
        if (i % 101 == 50) {
            // Param edit rebuild (drums module shared -> crossfade, render-once).
            rig.model.setParam(rig.model.patch().rhythm.drums.node, "kick_tune", static_cast<float>(i % 5));
            auto g = GraphBuilder::build(rig.model.patch(), defaultRegistry(), rig.engine.latestGraph(), 48000.0, 333);
            REQUIRE(g.reused > 0);
            rig.engine.publish(std::move(g.graph));
        }
        if (i % 50 == 0) rig.engine.collectGarbage();
        const float pk = rig.block(333);
        REQUIRE(std::isfinite(pk));
        peak = std::max(peak, pk);
    }
    REQUIRE(peak > 0.05f);
    REQUIRE(peak <= 1.0f);
    REQUIRE(rt::violationCount() == rt0);
}

TEST_CASE("engine: count-in clicks one bar before the first drum hit", "[transport][sequencer]") {
    EngineRig rig(64);
    Pattern p = allSteps();
    rig.engine.sequencer().setPattern(toRt(p));
    rig.engine.rhythm().countIn.store(true);
    rig.engine.transport().setPlaying(true);
    REQUIRE_FALSE(rig.engine.metronome().enabled());
    // During the count-in bar (2 s at 120): metronome clicks but the sequencer is still at step -1.
    float countPeak = 0.0f;
    for (int i = 0; i < 1400; ++i) countPeak = std::max(countPeak, rig.block(64)); // ~1.87 s
    REQUIRE(countPeak > 0.01f);
    REQUIRE(rig.engine.currentStep() == -1);
    for (int i = 0; i < 200; ++i) rig.block(64);
    REQUIRE(rig.engine.currentStep() >= 0);
}

TEST_CASE("render: --play-pattern style offline render", "[render][sequencer]") {
    const Pattern p = loadPatternFile("presets/patterns/take-on-me-80s-pop.json");
    RenderOptions opt;
    opt.pattern = &p;
    opt.patternBars = 2;
    opt.tailSeconds = 3.0;
    const RenderResult r = OfflineRenderer::render(PatchModel::makeDefaultPatch(), {}, opt);
    const double expected = 2 * 4 * 60.0 / 169.0 + 3.0;
    REQUIRE(std::fabs(r.seconds() - expected) < 0.01);
    float peak = 0.0f;
    for (float x : r.left) peak = std::max(peak, std::fabs(x));
    REQUIRE(peak > 0.05f);
    // tail decays
    double tail = 0.0;
    for (size_t i = r.left.size() - 24000; i < r.left.size(); ++i) tail = std::max(tail, static_cast<double>(std::fabs(r.left[i])));
    REQUIRE(tail < 1e-4);
}
