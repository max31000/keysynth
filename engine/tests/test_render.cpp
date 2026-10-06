// Render tests over every factory preset (ARCHITECTURE §12, docs/TESTING.md).
// Fast tier: 48 kHz / 64. Full tier ([.full], run with `ks-tests "[full]"`): {44.1, 48, 96} kHz x {32, 64, 512}.

#include "core/AppPaths.h"
#include "core/RtCheck.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace ks;

namespace {

struct Stats {
    bool finite = true;
    float peak = 0.0f;
    double dc = 0.0, rms = 0.0, tailRms = 0.0;
};

Stats analyze(const RenderResult& r, double tailFrom) {
    Stats s;
    double sum = 0.0, sq = 0.0, tsq = 0.0;
    size_t tn = 0;
    const size_t tailStart = static_cast<size_t>(tailFrom * r.sampleRate);
    for (size_t i = 0; i < r.left.size(); ++i) {
        for (float x : {r.left[i], r.right[i]}) {
            if (!std::isfinite(x)) {
                s.finite = false;
                continue;
            }
            s.peak = std::max(s.peak, std::fabs(x));
            sum += x;
            sq += static_cast<double>(x) * x;
            if (i >= tailStart) {
                tsq += static_cast<double>(x) * x;
                ++tn;
            }
        }
    }
    const double n = 2.0 * static_cast<double>(r.left.size());
    s.dc = n > 0 ? sum / n : 0.0;
    s.rms = n > 0 ? std::sqrt(sq / n) : 0.0;
    s.tailRms = tn > 0 ? std::sqrt(tsq / static_cast<double>(tn)) : 0.0;
    return s;
}

double db(double v) { return v > 1e-12 ? 20.0 * std::log10(v) : -240.0; }

void checkAllPresets(double sr, int block) {
    const PresetStore store(AppPaths::fromRoot(KS_SOURCE_DIR));
    const auto events = OfflineRenderer::standardTestEvents();
    double last = 0.0;
    for (const auto& e : events) last = std::max(last, e.time);
    int n = 0;
    for (const auto& p : store.list()) {
        if (!p.factory) continue;
        const Patch patch = store.load(p.path);
        // Sample-based presets need downloaded libraries (GBs): covered by the hidden [samples] test instead.
        if (std::any_of(patch.layers.begin(), patch.layers.end(),
                        [](const Layer& l) { return l.instrument.type == "sampler"; }))
            continue;
        ++n;
        INFO("preset " << p.path << " @ " << sr << " Hz / " << block);
        RenderOptions opt;
        opt.sampleRate = sr;
        opt.blockSize = block;
        opt.tailSeconds = 3.0;
        rt::resetViolations();
        const RenderResult r = OfflineRenderer::render(patch, events, opt);
        if (rt::checksEnabled()) REQUIRE(rt::violationCount() == 0);
        // Tail window: the last 0.5 s (notes released >= 2.5 s earlier).
        const Stats s = analyze(r, r.seconds() - 0.5);
        CAPTURE(s.peak, s.dc, db(s.rms), db(s.tailRms));
        REQUIRE(s.finite);                 // no NaN/Inf
        REQUIRE(s.peak <= 1.0f);           // <= 0 dBFS
        REQUIRE(std::fabs(s.dc) < 1e-3);   // DC below -60 dBFS
        REQUIRE(db(s.rms) > -40.0);        // not silent
        REQUIRE(db(s.tailRms) < -80.0);    // tails decay
    }
    REQUIRE(n >= 2);
}

} // namespace

TEST_CASE("Render: all factory presets, fast tier (48k/64)", "[render]") { checkAllPresets(48000.0, 64); }

TEST_CASE("Render: all factory presets, full tier", "[.full][render]") {
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int block : {32, 64, 512}) checkAllPresets(sr, block);
}

TEST_CASE("Render: note pitch matches (zero-crossing estimate)", "[render]") {
    // basic saw with low cutoff ~ sine-ish: count rising zero crossings of A4.
    const PresetStore store(AppPaths::fromRoot(KS_SOURCE_DIR));
    Patch patch = store.load("presets/factory/synth-lead/basic-saw-lead.json");
    patch.layers[0].instrument.params["cutoff"] = 600.0f;
    patch.layers[0].instrument.params["resonance"] = 0.0f;
    RenderOptions opt;
    opt.tailSeconds = 0.0;
    const auto r = OfflineRenderer::render(patch, OfflineRenderer::notesToEvents({{69, 0.0, 1.0, 100}}), opt);
    int crossings = 0;
    const size_t a = static_cast<size_t>(0.2 * r.sampleRate), b = static_cast<size_t>(0.9 * r.sampleRate);
    for (size_t i = a + 1; i < b; ++i)
        if (r.left[i - 1] < 0.0f && r.left[i] >= 0.0f) ++crossings;
    const double hz = crossings / 0.7;
    REQUIRE(std::fabs(hz - 440.0) < 4.0);
}

TEST_CASE("Render: split preset routes keys to layers with transpose", "[render]") {
    const PresetStore store(AppPaths::fromRoot(KS_SOURCE_DIR));
    const Patch patch = store.load("presets/factory/splits-layers/basic-bass-lead-split.json");
    RenderOptions opt;
    opt.tailSeconds = 0.5;
    // Key 40 (E2) is in the bass zone (transpose -12): sounds; layers have opposite pans.
    const auto r = OfflineRenderer::render(patch, OfflineRenderer::notesToEvents({{40, 0.0, 0.5, 100}}), opt);
    const Stats s = analyze(r, 10.0);
    REQUIRE(db(s.rms) > -40.0);
    double l = 0, rr = 0;
    for (size_t i = 0; i < r.left.size(); ++i) {
        l += std::fabs(r.left[i]);
        rr += std::fabs(r.right[i]);
    }
    REQUIRE(l > rr); // bass layer panned left
}

TEST_CASE("Note name parsing", "[render]") {
    REQUIRE(OfflineRenderer::parseNoteName("C4") == 60);
    REQUIRE(OfflineRenderer::parseNoteName("A4") == 69);
    REQUIRE(OfflineRenderer::parseNoteName("F#3") == 54);
    REQUIRE(OfflineRenderer::parseNoteName("Bb2") == 46);
    REQUIRE(OfflineRenderer::parseNoteName("C-1") == 0);
    REQUIRE(OfflineRenderer::parseNoteName("61") == 61);
    REQUIRE(OfflineRenderer::parseNoteName("H4") == -1);
    std::vector<NoteSpec> v;
    std::string err;
    REQUIRE(OfflineRenderer::parseNotes("C4:0:1,E4:0.5:1:80", v, err));
    REQUIRE(v.size() == 2);
    REQUIRE(v[1].velocity == 80);
    REQUIRE_FALSE(OfflineRenderer::parseNotes("C4:0", v, err));
}
