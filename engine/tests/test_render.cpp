// Render tests over every factory preset (ARCHITECTURE §12, docs/TESTING.md).
// Fast tier: 48 kHz / 64. Full tier ([.full], run with `ks-tests "[full]"`): {44.1, 48, 96} kHz x {32, 64, 512}.

#include "core/AppPaths.h"
#include "core/PatchModel.h"
#include "core/RtCheck.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

TEST_CASE("Controller / bend automation specs (ks-render --cc / --bend)", "[render]") {
    std::vector<TimedEvent> ev;
    std::string err;
    REQUIRE(OfflineRenderer::parseControllers("sustain:127:0, mod:127:1:0.5, 11:64:2", ev, err));
    // sustain step, mod ramp 0 -> 127 over 0.5 s (5 ms steps), expression step.
    REQUIRE(ev.size() == 1 + 100 + 1);
    REQUIRE(ev.front().event.type == MidiEventType::ControlChange);
    REQUIRE(ev.front().event.data1 == 64);
    int lastMod = -1;
    double lastModT = 0.0;
    for (const auto& e : ev)
        if (e.event.data1 == 1) {
            REQUIRE(static_cast<int>(e.event.value7) >= lastMod); // monotonic ramp
            lastMod = e.event.value7;
            lastModT = e.time;
        }
    REQUIRE(lastMod == 127);
    REQUIRE(std::fabs(lastModT - 1.5) < 1e-9);
    REQUIRE(ev.back().event.data1 == 11);
    REQUIRE(ev.back().event.value7 == 64);
    REQUIRE_FALSE(OfflineRenderer::parseControllers("mod:128:0", ev, err)); // value range
    REQUIRE_FALSE(OfflineRenderer::parseControllers("120:1:0", ev, err));   // channel-mode CCs refused
    REQUIRE_FALSE(OfflineRenderer::parseControllers("foo:1:0", ev, err));
    REQUIRE_FALSE(OfflineRenderer::parseControllers("1:1", ev, err));

    ev.clear();
    REQUIRE(OfflineRenderer::parseBends("1:0.5:0.1,0:1", ev, err));
    REQUIRE(ev.size() == 20 + 1);
    REQUIRE(ev[19].event.type == MidiEventType::PitchBend);
    REQUIRE(ev[19].event.valueF == 1.0f);
    REQUIRE(ev.back().event.valueF == 0.0f);
    REQUIRE_FALSE(OfflineRenderer::parseBends("1.5:0", ev, err));
    REQUIRE_FALSE(OfflineRenderer::parseBends("0.5:-1", ev, err));

    // A full bend on the basic synth (range 2 semitones) raises A4 to B4.
    const PresetStore store(AppPaths::fromRoot(KS_SOURCE_DIR));
    Patch patch = store.load("presets/factory/synth-lead/basic-saw-lead.json");
    patch.layers[0].instrument.params["cutoff"] = 600.0f;
    patch.layers[0].instrument.params["resonance"] = 0.0f;
    auto events = OfflineRenderer::notesToEvents({{69, 0.0, 1.0, 100}});
    REQUIRE(OfflineRenderer::parseBends("1:0", events, err));
    RenderOptions opt;
    opt.tailSeconds = 0.0;
    const auto r = OfflineRenderer::render(patch, events, opt);
    int crossings = 0;
    const size_t a = static_cast<size_t>(0.2 * r.sampleRate), b = static_cast<size_t>(0.9 * r.sampleRate);
    for (size_t i = a + 1; i < b; ++i)
        if (r.left[i - 1] < 0.0f && r.left[i] >= 0.0f) ++crossings;
    REQUIRE(std::fabs(crossings / 0.7 - 440.0 * std::exp2(2.0 / 12.0)) < 4.0);
}

TEST_CASE("Master volume_db applies before a trailing limiter", "[render][patch]") {
    // Hot basic patch, master limiter at -6 dB, +12 dB master volume: the limiter must catch the boost.
    Patch patch = PatchModel::makeDefaultPatch();
    ModuleSlot lim;
    lim.type = "limiter";
    lim.params["ceiling_db"] = -6.0f;
    patch.master.fx.push_back(lim);
    patch.master.volumeDb = 12.0f;
    RenderOptions opt;
    opt.tailSeconds = 0.2;
    const auto loud = OfflineRenderer::notesToEvents({{48, 0.0, 0.5, 127}, {60, 0.0, 0.5, 127}});
    const Stats s = analyze(OfflineRenderer::render(patch, loud, opt), 10.0);
    REQUIRE(db(s.peak) < -5.5);
    REQUIRE(db(s.peak) > -9.0); // boosted into the limiter, not attenuated after it

    // Without a limiter slot the volume is still applied (Engine safety limiter at the output).
    patch.master.fx.clear();
    const auto one = OfflineRenderer::notesToEvents({{60, 0.0, 0.5, 100}});
    patch.master.volumeDb = -20.0f;
    const double quiet = db(analyze(OfflineRenderer::render(patch, one, opt), 10.0).rms);
    patch.master.volumeDb = 0.0f;
    const double unity = db(analyze(OfflineRenderer::render(patch, one, opt), 10.0).rms);
    REQUIRE(std::fabs(unity - quiet - 20.0) < 0.5);

    // Out-of-range volume is clamped with a load warning.
    patch.master.volumeDb = 30.0f;
    PatchModel m(defaultRegistry());
    const auto w = m.setPatch(patch);
    REQUIRE(m.patch().master.volumeDb == PatchModel::kMaxVolumeDb);
    REQUIRE(std::any_of(w.begin(), w.end(), [](const std::string& x) { return x.find("clamped") != std::string::npos; }));
}
