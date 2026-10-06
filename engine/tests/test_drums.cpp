// `drums` instrument: pitch envelope, choke, all instruments x models finite and decaying.

#include "core/RtCheck.h"
#include "instruments/drums/DrumKit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ks;
using drums::Instr;

namespace {

struct Out {
    std::vector<float> l, r;
};

// Renders `seconds` with `events` (absolute sample positions) in blocks of 64.
Out render(DrumKit& k, std::vector<std::pair<int64_t, MidiEvent>> events, double seconds, double sr = 48000.0) {
    const int block = 64;
    const int64_t total = static_cast<int64_t>(seconds * sr);
    Out o;
    o.l.assign(static_cast<size_t>(total), 0.0f);
    o.r.assign(static_cast<size_t>(total), 0.0f);
    ProcessContext ctx;
    ctx.sampleRate = sr;
    ChannelState cs;
    ctx.channel = &cs;
    size_t ei = 0;
    std::vector<MidiEvent> blk;
    for (int64_t pos = 0; pos < total; pos += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
        blk.clear();
        while (ei < events.size() && events[ei].first < pos + n) {
            MidiEvent e = events[ei].second;
            e.sampleOffset = static_cast<uint32_t>(std::max<int64_t>(0, events[ei].first - pos));
            blk.push_back(e);
            ++ei;
        }
        AudioBlock b{o.l.data() + pos, o.r.data() + pos, n};
        ctx.numSamples = n;
        rt::RtScope scope;
        k.process(b, MidiEventSpan(blk.data(), blk.size()), ctx);
    }
    return o;
}

// Frequency estimate from zero crossings of a 1-pole low-passed copy in [t0, t1).
double zcFreq(const std::vector<float>& x, double sr, double t0, double t1) {
    const size_t a = static_cast<size_t>(t0 * sr), b = std::min(x.size(), static_cast<size_t>(t1 * sr));
    const float c = 1.0f - std::exp(-2.0f * 3.14159265f * 400.0f / static_cast<float>(sr));
    float y = 0.0f, prev = 0.0f;
    int crossings = 0;
    double first = -1, last = -1;
    for (size_t i = 0; i < b; ++i) {
        y += c * (x[i] - y);
        if (i >= a && i > 0 && prev <= 0.0f && y > 0.0f) {
            if (first < 0) first = static_cast<double>(i);
            last = static_cast<double>(i);
            ++crossings;
        }
        prev = y;
    }
    if (crossings < 2) return 0.0;
    return (crossings - 1) * sr / (last - first);
}

double rms(const std::vector<float>& x, double sr, double t0, double t1) {
    const size_t a = static_cast<size_t>(t0 * sr), b = std::min(x.size(), static_cast<size_t>(t1 * sr));
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}

} // namespace

TEST_CASE("drums: kick pitch envelope sweeps down to a plausible fundamental", "[drums][dsp]") {
    for (int kit = 0; kit < 4; ++kit) {
        DrumKit k;
        k.params().set("kit", static_cast<float>(kit));
        k.prepare(48000.0, 64);
        const Out o = render(k, {{0, MidiEvent::noteOn(36, 127, 10)}}, 0.4);
        const double early = zcFreq(o.l, 48000.0, 0.0005, 0.045);
        const double late = zcFreq(o.l, 48000.0, 0.08, 0.2);
        INFO("kit " << kit << " early " << early << " Hz, late " << late << " Hz");
        REQUIRE(late > 35.0);
        REQUIRE(late < 85.0);
        REQUIRE(early > late * 1.15);
    }
}

TEST_CASE("drums: kick tune param shifts the fundamental", "[drums][dsp]") {
    DrumKit a, b;
    b.params().set("kick_tune", 12.0f);
    a.prepare(48000.0, 64);
    b.prepare(48000.0, 64);
    const double fa = zcFreq(render(a, {{0, MidiEvent::noteOn(36, 127, 10)}}, 0.4).l, 48000.0, 0.1, 0.3);
    const double fb = zcFreq(render(b, {{0, MidiEvent::noteOn(36, 127, 10)}}, 0.4).l, 48000.0, 0.1, 0.3);
    INFO(fa << " -> " << fb);
    REQUIRE(fb / fa > 1.8);
    REQUIRE(fb / fa < 2.2);
}

TEST_CASE("drums: closed hat chokes the open hat", "[drums][dsp]") {
    for (int kit = 0; kit < 4; ++kit) {
        DrumKit open, choked;
        open.params().set("kit", static_cast<float>(kit));
        choked.params().set("kit", static_cast<float>(kit));
        open.prepare(48000.0, 64);
        choked.prepare(48000.0, 64);
        const Out a = render(open, {{0, MidiEvent::noteOn(46, 110, 10)}}, 0.5);
        const Out b = render(choked, {{0, MidiEvent::noteOn(46, 110, 10)}, {4800, MidiEvent::noteOn(42, 1, 10)}}, 0.5);
        // 100 ms after the closed hat: open-hat ring is gone (closed hat at velocity 1 is ~-40 dB and short).
        const double ra = rms(a.l, 48000.0, 0.2, 0.3), rb = rms(b.l, 48000.0, 0.2, 0.3);
        INFO("kit " << kit << " open " << ra << " choked " << rb);
        REQUIRE(ra > 1e-3);
        REQUIRE(rb < ra * 0.05);
        for (int s = 0; s < DrumKit::kSlotsPerInstr; ++s) REQUIRE_FALSE(choked.voice(Instr::OpenHat, s).active());
    }
}

TEST_CASE("drums: velocity scales level", "[drums][dsp]") {
    DrumKit a, b;
    a.prepare(48000.0, 64);
    b.prepare(48000.0, 64);
    const double loud = rms(render(a, {{0, MidiEvent::noteOn(38, 127, 10)}}, 0.2).l, 48000.0, 0.0, 0.1);
    const double soft = rms(render(b, {{0, MidiEvent::noteOn(38, 30, 10)}}, 0.2).l, 48000.0, 0.0, 0.1);
    REQUIRE(soft < loud * 0.4);
    REQUIRE(soft > 0.0);
}

TEST_CASE("drums: every instrument x model is finite, bounded, decays to silence, RT-safe", "[drums][dsp][rt]") {
    const uint64_t rt0 = rt::violationCount();
    for (int kit = 0; kit < 4; ++kit) {
        for (float extreme : {0.0f, 0.5f, 1.0f}) {
            DrumKit k;
            k.params().set("kit", static_cast<float>(kit));
            for (int i = 0; i < drums::kNumInstr; ++i) {
                k.params().set(DrumKit::paramIndex(static_cast<Instr>(i), DrumKit::VDecay), extreme);
                k.params().set(DrumKit::paramIndex(static_cast<Instr>(i), DrumKit::VTone), extreme);
                k.params().set(DrumKit::paramIndex(static_cast<Instr>(i), DrumKit::VTune), (extreme - 0.5f) * 24.0f);
                k.params().set(DrumKit::paramIndex(static_cast<Instr>(i), DrumKit::VLevel), 6.0f);
            }
            k.prepare(48000.0, 64);
            std::vector<std::pair<int64_t, MidiEvent>> ev;
            int64_t t = 0;
            for (int note = 30; note <= 82; ++note) {
                ev.push_back({t, MidiEvent::noteOn(note, 127, 10)});
                t += 480; // 10 ms apart -> heavy overlap
            }
            const double secs = extreme > 0.75f ? 13.0 : 6.5; // decay 1 = 2.8x longer
            const Out o = render(k, ev, secs);
            float peak = 0.0f;
            bool finite = true;
            for (size_t i = 0; i < o.l.size(); ++i) {
                finite = finite && std::isfinite(o.l[i]) && std::isfinite(o.r[i]);
                peak = std::max({peak, std::fabs(o.l[i]), std::fabs(o.r[i])});
            }
            INFO("kit " << kit << " extreme " << extreme << " peak " << peak);
            REQUIRE(finite);
            REQUIRE(peak < 16.0f); // ~50 overlapping hits at +6 dB: bounded (the limiter follows in the engine)
            REQUIRE(peak > 0.05f);
            REQUIRE(k.activeVoices() == 0); // every hit ends (end window = 6 x longest time constant)
            REQUIRE(rms(o.l, 48000.0, secs - 0.5, secs) < 1e-6);
        }
    }
    REQUIRE(rt::violationCount() == rt0);
}

TEST_CASE("drums: note map (GM + octave fold)", "[drums]") {
    REQUIRE(DrumKit::mapNote(36).instr == Instr::Kick);
    REQUIRE(DrumKit::mapNote(38).instr == Instr::Snare);
    REQUIRE(DrumKit::mapNote(39).instr == Instr::Clap);
    REQUIRE(DrumKit::mapNote(42).instr == Instr::ClosedHat);
    REQUIRE(DrumKit::mapNote(46).instr == Instr::OpenHat);
    REQUIRE(DrumKit::mapNote(49).instr == Instr::Crash);
    REQUIRE(DrumKit::mapNote(51).instr == Instr::Ride);
    REQUIRE(DrumKit::mapNote(45).instr == Instr::TomLo);
    REQUIRE(DrumKit::mapNote(47).instr == Instr::TomMid);
    REQUIRE(DrumKit::mapNote(50).instr == Instr::TomHi);
    REQUIRE(DrumKit::mapNote(37).instr == Instr::Rim);
    REQUIRE(DrumKit::mapNote(56).instr == Instr::Cowbell);
    REQUIRE(DrumKit::mapNote(54).instr == Instr::Tamb);
    REQUIRE(DrumKit::mapNote(60).instr == Instr::Kick);  // 60 folds onto 36
    REQUIRE(DrumKit::mapNote(62).instr == Instr::Snare); // 62 -> 38
    REQUIRE(DrumKit::mapNote(0).instr == Instr::Kick);
    REQUIRE(DrumKit::mapNote(127).instr == Instr::TomLo); // 127 -> 43 (high floor tom)
}
