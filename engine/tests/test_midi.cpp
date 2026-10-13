// MIDI processors (ARCHITECTURE §17): arp timing across block sizes, modes, latch, chord tool, hold, chain changes.

#include "core/Engine.h"
#include "core/GraphBuilder.h"
#include "core/ModuleRegistry.h"
#include "core/PatchModel.h"
#include "core/RtCheck.h"
#include "midi/arp/Arpeggiator.h"
#include "midi/chord/ChordTool.h"
#include "midi/hold/HoldTool.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <vector>

using namespace ks;

namespace {

struct Ev {
    int64_t at;
    MidiEventType type;
    int note;
    int vel;
    bool operator==(const Ev&) const = default;
};

struct Clock {
    double sr = 48000.0;
    double tempo = 120.0;
    bool playing = false;
};

// Runs `m` over [0, total) in blocks of `block`, feeding `input` (absolute times) and collecting the output with
// absolute times. Transport ppq = sampleTime * tempo / 60 / sr when playing.
std::vector<Ev> run(Module& m, const std::vector<Ev>& input, int64_t total, int block, const Clock& c,
                    const std::map<int64_t, std::pair<std::string, float>>& paramAt = {}) {
    m.prepare(c.sr, block);
    std::vector<Ev> out;
    std::vector<MidiEvent> in;
    std::vector<MidiEvent> buf(512);
    ChannelState cs;
    size_t next = 0;
    for (int64_t t0 = 0; t0 < total; t0 += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, total - t0));
        for (const auto& [at, pv] : paramAt)
            if (at >= t0 && at < t0 + n) m.params().set(pv.first, pv.second);
        in.clear();
        while (next < input.size() && input[next].at < t0 + n) {
            const Ev& e = input[next++];
            MidiEvent x;
            x.type = e.type;
            x.data1 = static_cast<uint8_t>(e.note);
            x.value7 = static_cast<uint8_t>(e.vel);
            x.channel = 1;
            x.sampleOffset = static_cast<uint32_t>(e.at - t0);
            in.push_back(x);
        }
        ProcessContext ctx;
        ctx.sampleRate = c.sr;
        ctx.numSamples = n;
        ctx.sampleTime = t0;
        ctx.transport.tempo = c.tempo;
        ctx.transport.playing = c.playing;
        ctx.transport.ppqPosition = c.playing ? static_cast<double>(t0) * c.tempo / 60.0 / c.sr : 0.0;
        ctx.channel = &cs;
        MidiEventBuffer ob(buf.data(), buf.size());
        m.processMidi(MidiEventSpan(in.data(), in.size()), ob, ctx);
        ob.sortByOffset();
        for (size_t i = 0; i < ob.size(); ++i) {
            const MidiEvent& e = ob.data()[i];
            REQUIRE(e.sampleOffset < static_cast<uint32_t>(n));
            out.push_back({t0 + e.sampleOffset, e.type, e.data1, e.value7});
        }
    }
    return out;
}

std::vector<Ev> ons(const std::vector<Ev>& v) {
    std::vector<Ev> o;
    for (const auto& e : v)
        if (e.type == MidiEventType::NoteOn) o.push_back(e);
    return o;
}

std::vector<int> notesOf(const std::vector<Ev>& v) {
    std::vector<int> o;
    for (const auto& e : v) o.push_back(e.note);
    return o;
}

Ev on(int64_t at, int note, int vel = 100) { return {at, MidiEventType::NoteOn, note, vel}; }
Ev off(int64_t at, int note) { return {at, MidiEventType::NoteOff, note, 0}; }

void setArp(Arpeggiator& a, std::initializer_list<std::pair<const char*, float>> ps) {
    for (const auto& [id, v] : ps) REQUIRE(a.params().set(id, v));
}

} // namespace

TEST_CASE("arp: own clock is sample-accurate for any block size", "[midi][arp]") {
    const Clock c{48000.0, 120.0, false};
    const std::vector<Ev> in = {on(1000, 60), on(1000, 64), on(1000, 67), off(40000, 60), off(40000, 64), off(40000, 67)};
    std::vector<Ev> ref;
    for (int block : {32, 64, 333}) {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}, {"gate", 0.5f}}); // 1/16 at 120 BPM = 6000 samples
        const auto out = run(a, in, 60000, block, c);
        if (ref.empty()) ref = out;
        INFO("block " << block);
        REQUIRE(out == ref);
    }
    const auto o = ons(ref);
    REQUIRE(o.size() == 7); // steps at 1000 + k*6000 < 40000
    for (size_t k = 0; k < o.size(); ++k) {
        CHECK(o[k].at == 1000 + static_cast<int64_t>(k) * 6000);
        CHECK(o[k].note == std::vector<int>{60, 64, 67}[k % 3]);
    }
    // gate 0.5: each note-off 3000 samples after its note-on; nothing after the keys are released except the tail
    for (const auto& e : ref)
        if (e.type == MidiEventType::NoteOff) CHECK((e.at - 1000) % 6000 == 3000);
}

TEST_CASE("arp: transport grid timing, identical across block sizes", "[midi][arp]") {
    const Clock c{48000.0, 133.0, true}; // fractional step lengths
    const std::vector<Ev> in = {on(1000, 60), on(1500, 63), off(70000, 60), off(70000, 63)};
    std::vector<Ev> ref;
    for (int block : {32, 64, 333}) {
        Arpeggiator a;
        setArp(a, {{"sync", 10.0f}, {"gate", 0.8f}, {"swing", 0.3f}}); // 1/8
        const auto out = run(a, in, 80000, block, c);
        if (ref.empty()) ref = out;
        INFO("block " << block);
        REQUIRE(out == ref);
    }
    const double spp = 60.0 * 48000.0 / 133.0;
    const auto o = ons(ref);
    REQUIRE(o.size() >= 6);
    // First step: the next 1/8 grid point after the press (k = 1); swing delays odd grid steps.
    for (size_t i = 0; i < o.size(); ++i) {
        const int64_t k = static_cast<int64_t>(i) + 1;
        const double exact = (static_cast<double>(k) * 0.5 + ((k & 1) ? 0.3 * 0.25 : 0.0)) * spp;
        CHECK(o[i].at == static_cast<int64_t>(std::ceil(exact - 1e-4)));
    }
}

TEST_CASE("arp: modes and octaves", "[midi][arp]") {
    const Clock c{48000.0, 120.0, false};
    // Pressed G, C, E (as-played order differs from pitch order).
    const std::vector<Ev> in = {on(0, 67), on(0, 60), on(0, 64)};
    auto seq = [&](int mode, int octaves, int steps) {
        Arpeggiator a;
        setArp(a, {{"mode", static_cast<float>(mode)}, {"octaves", static_cast<float>(octaves)}, {"sync", 13.0f}});
        return notesOf(ons(run(a, in, 6000 * steps - 1, 64, c)));
    };
    CHECK(seq(Arpeggiator::Up, 1, 4) == std::vector<int>{60, 64, 67, 60});
    CHECK(seq(Arpeggiator::Down, 1, 4) == std::vector<int>{67, 64, 60, 67});
    CHECK(seq(Arpeggiator::UpDown, 1, 6) == std::vector<int>{60, 64, 67, 64, 60, 64});
    CHECK(seq(Arpeggiator::AsPlayed, 1, 4) == std::vector<int>{67, 60, 64, 67});
    CHECK(seq(Arpeggiator::Up, 2, 7) == std::vector<int>{60, 64, 67, 72, 76, 79, 60});
    // Chord: all notes each step, octave alternating.
    {
        Arpeggiator a;
        setArp(a, {{"mode", static_cast<float>(Arpeggiator::Chord)}, {"octaves", 2.0f}, {"sync", 13.0f}});
        const auto o = ons(run(a, in, 12000 - 1, 64, c));
        REQUIRE(o.size() == 6);
        CHECK(o[0].at == 0);
        CHECK(o[3].at == 6000);
        std::vector<int> second = {o[3].note, o[4].note, o[5].note};
        std::sort(second.begin(), second.end());
        CHECK(second == std::vector<int>{72, 76, 79});
    }
    // Random: deterministic per instance, never the same index twice in a row.
    {
        Arpeggiator a, b;
        setArp(a, {{"mode", static_cast<float>(Arpeggiator::Random)}, {"sync", 16.0f}});
        setArp(b, {{"mode", static_cast<float>(Arpeggiator::Random)}, {"sync", 16.0f}});
        const auto ra = notesOf(ons(run(a, in, 48000, 64, c)));
        const auto rb = notesOf(ons(run(b, in, 48000, 333, c)));
        REQUIRE(ra == rb);
        REQUIRE(ra.size() > 10);
        for (size_t i = 1; i < ra.size(); ++i) CHECK(ra[i] != ra[i - 1]);
    }
}

TEST_CASE("arp: latch, retrigger, legato, velocity, pass-through", "[midi][arp]") {
    const Clock c{48000.0, 120.0, false};
    SECTION("latch keeps running and a new chord replaces it") {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}, {"latch", 1.0f}});
        const std::vector<Ev> in = {on(0, 60), on(0, 64), off(100, 60), off(100, 64), on(30000, 70), off(30100, 70)};
        const auto o = ons(run(a, in, 48000, 64, c));
        // 0, 6000, 12000, 18000, 24000 from the latched C/E; then 70 only from 30000 (retrigger at the press).
        REQUIRE(o.size() == 8);
        CHECK(notesOf({o.begin(), o.begin() + 5}) == std::vector<int>{60, 64, 60, 64, 60});
        CHECK(o[5].at == 30000);
        CHECK(o[5].note == 70);
        CHECK(o[7].note == 70);
    }
    SECTION("without latch the arp stops when keys are released") {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}});
        const auto o = ons(run(a, {on(0, 60), off(7000, 60)}, 48000, 64, c));
        CHECK(o.size() == 2);
    }
    SECTION("legato gate: the off lands on the next step's sample, before its note-on") {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}, {"gate", 1.0f}});
        const auto out = run(a, {on(0, 60), on(0, 64)}, 13000, 32, c);
        REQUIRE(out.size() >= 4);
        CHECK(out[0] == on(0, 60, 100));
        CHECK(out[1].type == MidiEventType::NoteOff);
        CHECK(out[1].at == 6000);
        CHECK(out[2] == on(6000, 64, 100));
    }
    SECTION("fixed and accent velocity") {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}, {"vel_mode", 2.0f}, {"velocity", 60.0f}, {"accent_every", 3.0f}});
        const auto o = ons(run(a, {on(0, 60, 20)}, 6000 * 6 - 1, 64, c));
        REQUIRE(o.size() == 6);
        CHECK(o[0].vel == 127);
        CHECK(o[1].vel == 60);
        CHECK(o[3].vel == 127);
        Arpeggiator b;
        setArp(b, {{"sync", 13.0f}, {"vel_mode", 1.0f}, {"velocity", 90.0f}});
        CHECK(ons(run(b, {on(0, 60, 20)}, 6000, 64, c))[0].vel == 90);
    }
    SECTION("unknown note-offs and controllers pass through; AllNotesOff releases") {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}, {"gate", 0.9f}});
        std::vector<Ev> in = {off(10, 50), on(100, 60), {200, MidiEventType::AllNotesOff, 0, 0}};
        const auto out = run(a, in, 20000, 64, c);
        REQUIRE(out.size() == 4);
        CHECK(out[0] == off(10, 50));
        CHECK(out[1] == on(100, 60, 100));
        CHECK(out[2] == off(200, 60));
        CHECK(out[3].type == MidiEventType::AllNotesOff);
    }
    SECTION("tempo change re-anchors at the last step") {
        Arpeggiator a;
        setArp(a, {{"sync", 13.0f}});
        Clock c2 = c;
        // Run 2 steps at 120 BPM, then switch the rate param (free) mid-way: steps continue from the last one.
        std::map<int64_t, std::pair<std::string, float>> pa = {{7000, {"sync", 7.0f}}}; // 1/4 = 24000
        const auto o = ons(run(a, {on(0, 60)}, 60000, 64, c2, pa));
        REQUIRE(o.size() >= 3);
        CHECK(o[1].at == 6000);
        CHECK(o[2].at == 30000); // last step 6000 + 24000
    }
}

TEST_CASE("chord: tables, voicings, refcount, memory", "[midi][chord]") {
    std::array<int, ChordTool::kMaxTones> t{};
    int mem[8] = {0, 3, 7, 10, 0, 0, 0, 0};
    REQUIRE(ChordTool::chordTones(60, ChordTool::Major, ChordTool::Close, 0, mem, 0, t) == 3);
    CHECK((std::vector<int>{t[0], t[1], t[2]}) == std::vector<int>{60, 64, 67});
    REQUIRE(ChordTool::chordTones(60, ChordTool::Major, ChordTool::Close, 1, mem, 0, t) == 3);
    CHECK((std::vector<int>{t[0], t[1], t[2]}) == std::vector<int>{64, 67, 72});
    REQUIRE(ChordTool::chordTones(60, ChordTool::Min7, ChordTool::Open, 0, mem, 0, t) == 4);
    CHECK((std::vector<int>{t[0], t[1], t[2], t[3]}) == std::vector<int>{60, 67, 75, 82});
    REQUIRE(ChordTool::chordTones(60, ChordTool::Major, ChordTool::Wide, 0, mem, 0, t) == 4);
    CHECK(t[0] == 48);
    REQUIRE(ChordTool::chordTones(62, ChordTool::Memory, ChordTool::Close, 0, mem, 4, t) == 4);
    CHECK((std::vector<int>{t[0], t[1], t[2], t[3]}) == std::vector<int>{62, 65, 69, 72});
    REQUIRE(ChordTool::chordTones(121, ChordTool::Major, ChordTool::Close, 0, mem, 0, t) == 2); // 127 cap

    const Clock c{};
    ChordTool ch;
    // C major and A major share E (64): one note-on for it, the off when the last of the two keys is released.
    REQUIRE(ch.params().set("type", static_cast<float>(ChordTool::Major)));
    const auto out = run(ch, {on(0, 60), on(10, 57), off(20, 60), off(30, 57)}, 64, 64, c);
    // C: 60 64 67 on; A major (57 61 64): 57 61 on (64 already sounding); off 60 67 at 20; off 57 61 64 at 30
    int ons64 = 0, offs64 = 0;
    for (const auto& e : out)
        if (e.note == 64) (e.type == MidiEventType::NoteOn ? ons64 : offs64)++;
    CHECK(ons64 == 1);
    CHECK(offs64 == 1);
    for (const auto& e : out)
        if (e.note == 64 && e.type == MidiEventType::NoteOff) CHECK(e.at == 30);
    // Held readouts for chord_learn.
    ChordTool h;
    run(h, {on(0, 64), on(0, 60), on(0, 67)}, 64, 64, c);
    CHECK(h.params().get(ChordTool::HeldCount) == 3.0f);
    CHECK(h.params().get(ChordTool::Held1) == 60.0f);
    CHECK(h.params().get(ChordTool::Held1 + 2) == 67.0f);
    CHECK(h.params().get(ChordTool::Held1 + 3) == -1.0f);
}

TEST_CASE("hold: sustain, latch and pedal", "[midi][hold]") {
    const Clock c{};
    {
        HoldTool h; // hold on, sustain mode
        const auto out = run(h, {on(0, 60), off(10, 60), on(20, 60), off(30, 60)}, 64, 64, c);
        // off deferred; re-strike: off + on at 20
        REQUIRE(out.size() == 3);
        CHECK(out[0] == on(0, 60, 100));
        CHECK(out[1] == off(20, 60));
        CHECK(out[2] == on(20, 60, 100));
        // turning hold off releases
        h.params().set("hold", 0.0f);
        std::vector<MidiEvent> buf(16);
        MidiEventBuffer ob(buf.data(), buf.size());
        ProcessContext ctx;
        ctx.numSamples = 64;
        h.processMidi({}, ob, ctx);
        REQUIRE(ob.size() == 1);
        CHECK(ob.data()[0].type == MidiEventType::NoteOff);
    }
    {
        HoldTool h;
        h.params().set("mode", static_cast<float>(HoldTool::Latch));
        const auto out = run(h, {on(0, 60), on(0, 64), off(10, 60), off(10, 64), on(20, 62), off(30, 62)}, 64, 64, c);
        // the new key after all were up releases 60 and 64
        int offsAt20 = 0;
        for (const auto& e : out)
            if (e.type == MidiEventType::NoteOff && e.at == 20) ++offsAt20;
        CHECK(offsAt20 == 2);
    }
    {
        HoldTool h;
        h.params().set("pedal", 1.0f);
        std::vector<Ev> in = {{0, MidiEventType::ControlChange, 64, 127}, on(5, 60), off(10, 60),
                              {20, MidiEventType::ControlChange, 64, 0}};
        const auto out = run(h, in, 64, 64, c);
        REQUIRE(out.size() == 2); // CC64 consumed
        CHECK(out[0] == on(5, 60, 100));
        CHECK(out[1] == off(20, 60));
    }
}

namespace {
void publish(Engine& e, PatchModel& m, bool reuse = true) {
    BuildResult r = GraphBuilder::build(m.patch(), defaultRegistry(), reuse ? e.latestGraph() : nullptr, e.sampleRate(),
                                        e.maxBlock());
    e.publish(std::move(r.graph));
}
} // namespace

TEST_CASE("midi chain in the engine: arp drives the instrument, no RT violations, chain edits release", "[midi][rt]") {
    rt::resetViolations();
    Engine e;
    e.prepare(48000.0, 64);
    PatchModel m(defaultRegistry());
    const NodeId layer = m.patch().layers[0].node;
    const NodeId arp = m.addFx(layer, "arp", std::nullopt);
    REQUIRE(m.patch().layers[0].midi.size() == 1);
    REQUIRE(m.patch().layers[0].fx.empty());
    REQUIRE_THROWS_AS(m.addFx(kMasterNode, "arp", std::nullopt), PatchError);
    m.setParam(arp, "sync", 13.0f);
    m.setParam(arp, "latch", 1.0f);
    publish(e, m);
    std::vector<float> l(64), r(64);
    auto block = [&](std::vector<MidiEvent> ev = {}) {
        AudioBlock ab{l.data(), r.data(), 64};
        e.processBlock(ab, MidiEventSpan(ev.data(), ev.size()));
        float pk = 0;
        for (int i = 0; i < 64; ++i) pk = std::max(pk, std::fabs(l[i]));
        return pk;
    };
    block({MidiEvent::noteOn(60, 100, 1, 0), MidiEvent::noteOn(64, 100, 1, 0)});
    block({MidiEvent::noteOff(60, 0, 1, 0), MidiEvent::noteOff(64, 0, 1, 0)});
    float peak = 0;
    for (int i = 0; i < 400; ++i) peak = std::max(peak, block()); // latched arp keeps playing
    CHECK(peak > 0.01f);
    CHECK(e.telemetry().voices.load() > 0);
    // Bypass: the arp's notes are released, the layer goes quiet.
    if (RackGraph* g = e.latestGraph()) g->findModuleNode(arp)->bypass.store(true);
    m.setFxBypass(arp, true);
    for (int i = 0; i < 1500; ++i) block();
    CHECK(e.telemetry().voices.load() == 0);
    // Un-bypass + removal while sounding: the new graph releases the layer once.
    m.setFxBypass(arp, false);
    publish(e, m);
    for (int i = 0; i < 50; ++i) block({});
    block({MidiEvent::noteOn(62, 100, 1, 0)});
    for (int i = 0; i < 100; ++i) block();
    m.removeFx(arp);
    publish(e, m);
    for (int i = 0; i < 1500; ++i) block();
    CHECK(e.telemetry().voices.load() == 0);
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
}
