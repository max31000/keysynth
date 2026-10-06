// `fm` instrument (DX7 / MSFA): .syx parsing, param <-> voice mapping, pitch, algorithms, extremes, voices.

#include "core/AppPaths.h"
#include "core/RtCheck.h"
#include "instruments/fm/Dx7Voice.h"
#include "instruments/fm/FmSynth.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <vector>

using namespace ks;
using namespace ks::fm;

namespace {

constexpr double kSr = 48000.0;

struct TimedEvent {
    int64_t at; // sample position
    MidiEvent e;
};

struct Render {
    std::vector<float> left, right;
    int maxActive = 0;
    int finalActive = 0;
};

// Drives the module like the Engine does (sorted events per block, ChannelState kept in sync, RtScope).
Render render(FmSynth& m, std::vector<TimedEvent> evs, int64_t total, int block = 64, ChannelState* chan = nullptr,
              double sr = kSr) {
    std::stable_sort(evs.begin(), evs.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.at < b.at; });
    Render r;
    r.left.assign(static_cast<size_t>(total), 0.0f);
    r.right.assign(static_cast<size_t>(total), 0.0f);
    ChannelState localState;
    ChannelState& cs = chan ? *chan : localState;
    std::vector<MidiEvent> blockEvents;
    blockEvents.reserve(evs.size());
    size_t next = 0;
    for (int64_t pos = 0; pos < total; pos += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
        blockEvents.clear();
        while (next < evs.size() && evs[next].at < pos + n) {
            MidiEvent e = evs[next].e;
            e.sampleOffset = static_cast<uint32_t>(std::max<int64_t>(0, evs[next].at - pos));
            cs.apply(e);
            blockEvents.push_back(e);
            ++next;
        }
        AudioBlock b{r.left.data() + pos, r.right.data() + pos, n};
        ProcessContext ctx;
        ctx.sampleRate = sr;
        ctx.numSamples = n;
        ctx.sampleTime = pos;
        ctx.channel = &cs;
        {
            rt::RtScope scope;
            m.process(b, MidiEventSpan(blockEvents.data(), blockEvents.size()), ctx);
        }
        r.maxActive = std::max(r.maxActive, m.activeVoices());
    }
    r.finalActive = m.activeVoices();
    return r;
}

int64_t sec(double s) { return static_cast<int64_t>(s * kSr); }

TimedEvent on(double t, int note, int vel = 100) { return {sec(t), MidiEvent::noteOn(note, vel)}; }
TimedEvent off(double t, int note) { return {sec(t), MidiEvent::noteOff(note)}; }
TimedEvent cc(double t, int num, int val) { return {sec(t), MidiEvent::cc(num, val)}; }

bool allFinite(const std::vector<float>& x) {
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}
float peakOf(const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX) {
    float p = 0.0f;
    for (size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}
double rmsOf(const std::vector<float>& x, size_t from, size_t to) {
    double s = 0.0;
    to = std::min(to, x.size());
    for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(s / static_cast<double>(to - from)) : 0.0;
}

// Frequency of a (near) sine by interpolated rising zero crossings.
double measureFreq(const std::vector<float>& x, size_t from, size_t to, double sr = kSr) {
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0) first = t;
            else ++count;
            last = t;
        }
    }
    return count > 0 ? sr * count / (last - first) : 0.0;
}
double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
double noteHz(int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }

void setP(FmSynth& m, const char* id, float v) { REQUIRE(m.params().set(id, v)); }

Dx7Voice randomVoice(std::mt19937& rng) {
    Dx7Voice v;
    std::uniform_int_distribution<int> byte(0, 127);
    for (auto& b : v.data) b = static_cast<uint8_t>(byte(rng));
    v.sanitize();
    std::uniform_int_distribution<int> ch('A', 'Z');
    std::string name;
    for (int i = 0; i < 10; ++i) name.push_back(static_cast<char>(ch(rng)));
    v.setName(name);
    return v;
}

} // namespace

TEST_CASE("fm: DX7 checksum and packed voice round trip", "[fm]") {
    const std::vector<uint8_t> d = {1, 2, 3, 120};
    CHECK(dx7Checksum(d) == ((128 - (126 & 127)) & 127));
    std::mt19937 rng(7);
    for (int i = 0; i < 200; ++i) {
        const Dx7Voice v = randomVoice(rng);
        std::array<uint8_t, 128> packed{};
        v.toPacked(packed);
        for (uint8_t b : packed) REQUIRE(b < 128);
        const Dx7Voice back = Dx7Voice::fromPacked(packed);
        REQUIRE(std::equal(v.data.begin(), v.data.begin() + vced::kSize, back.data.begin()));
    }
}

TEST_CASE("fm: parse 32-voice bulk dump and single voice dump", "[fm]") {
    std::mt19937 rng(11);
    std::vector<Dx7Voice> voices;
    for (int i = 0; i < 32; ++i) voices.push_back(randomVoice(rng));
    const std::vector<uint8_t> bulk = encodeBulk(voices, 3);
    REQUIRE(bulk.size() == 4104);
    CHECK(bulk[0] == 0xf0);
    CHECK(bulk[2] == 0x03);
    CHECK(bulk.back() == 0xf7);
    CHECK(bulk[4102] == dx7Checksum(std::span<const uint8_t>(bulk.data() + 6, 4096)));

    SyxParseResult r = parseSyx(bulk);
    REQUIRE(r.error.empty());
    CHECK(r.checksumOk);
    REQUIRE(r.voices.size() == 32);
    for (size_t i = 0; i < 32; ++i) {
        CHECK(r.voices[i].name() == voices[i].name());
        CHECK(std::equal(voices[i].data.begin(), voices[i].data.begin() + vced::kSize, r.voices[i].data.begin()));
    }

    // Bad checksum: still loaded, flagged.
    std::vector<uint8_t> bad = bulk;
    bad[4102] = static_cast<uint8_t>((bad[4102] + 1) & 0x7f);
    r = parseSyx(bad);
    CHECK_FALSE(r.checksumOk);
    CHECK(r.voices.size() == 32);

    // Raw 4096-byte bank.
    r = parseSyx(std::span<const uint8_t>(bulk.data() + 6, 4096));
    CHECK(r.voices.size() == 32);

    // Single voice dump (VCED) + concatenation.
    const std::vector<uint8_t> single = encodeSingle(voices[5]);
    REQUIRE(single.size() == 163);
    r = parseSyx(single);
    REQUIRE(r.voices.size() == 1);
    CHECK(r.checksumOk);
    CHECK(r.voices[0].name() == voices[5].name());
    std::vector<uint8_t> both = single;
    both.insert(both.end(), bulk.begin(), bulk.end());
    CHECK(parseSyx(both).voices.size() == 33);

    // Garbage / truncated input.
    CHECK_FALSE(parseSyx(std::vector<uint8_t>{0xf0, 0x43, 0x00, 0x09, 0x20, 0x00, 1, 2}).error.empty());
    CHECK_FALSE(parseSyx(std::vector<uint8_t>{0xf0, 0x41, 0x10, 0x42, 0x12, 0x40}).error.empty());
    CHECK_FALSE(parseSyx(std::vector<uint8_t>{}).error.empty());

    // Out-of-range bytes are clamped.
    std::vector<uint8_t> wild = single;
    for (size_t i = 6; i < 6 + 155; ++i) wild[i] = 0x7f;
    r = parseSyx(wild);
    REQUIRE(r.voices.size() == 1);
    CHECK(r.voices[0].data[vced::Algorithm] == 31);
    CHECK(r.voices[0].data[vced::opBase(1) + vced::Detune] == 14);
}

TEST_CASE("fm: param <-> voice round trip", "[fm]") {
    FmSynth m;
    CHECK(m.params().size() == FmSynth::kParamCount);
    CHECK(m.params().indexOf("alg") == FmSynth::Alg);
    CHECK(m.params().indexOf("op1_level") == FmSynth::opParam(1, FmSynth::OpOutLevel));
    CHECK(m.params().indexOf("op6_on") == FmSynth::kParamCount - 1);
    // Defaults = DX7 INIT VOICE.
    const Dx7Voice init = Dx7Voice::initVoice();
    CHECK(std::equal(init.data.begin(), init.data.begin() + vced::Name, FmSynth::paramsToVoice(m.params()).data.begin()));

    std::mt19937 rng(3);
    for (int i = 0; i < 50; ++i) {
        const Dx7Voice v = randomVoice(rng);
        m.applyVoice(v);
        const Dx7Voice back = FmSynth::paramsToVoice(m.params());
        REQUIRE(std::equal(v.data.begin(), v.data.begin() + vced::Name, back.data.begin()));
    }
    // DX7 numbering: alg param 1..32 = byte 0..31, detune -7..7 = 0..14, transpose -24..24 = 0..48, OP6 first.
    m.params().resetToDefaults();
    setP(m, "alg", 32);
    setP(m, "op6_detune", -7);
    setP(m, "op1_coarse", 31);
    setP(m, "transpose", 12);
    const Dx7Voice v = FmSynth::paramsToVoice(m.params());
    CHECK(v.data[vced::Algorithm] == 31);
    CHECK(v.data[0 + vced::Detune] == 0);           // OP6 block first
    CHECK(v.data[5 * 21 + vced::Coarse] == 31);     // OP1 block last
    CHECK(v.data[vced::Transpose] == 36);
}

TEST_CASE("fm: loadState loads a voice from a .syx bank into the ParamSet", "[fm]") {
    const AppPaths paths = AppPaths::discover();
    const std::string rel = "assets/dx7/keysynth-fm-factory.syx";
    const SyxParseResult bank = loadSyxFile(paths.root / rel);
    REQUIRE(bank.error.empty());
    CHECK(bank.checksumOk);
    REQUIRE(bank.voices.size() == 32);
    CHECK(bank.voices[0].name() == "BALLAD EP");
    CHECK(bank.voices[31].name() == "INIT VOICE");

    // The bank voice equals the self-contained factory preset's params.
    std::ifstream pf(paths.root / "presets/factory/e-piano/fm-ballad-epiano.json");
    const auto preset = nlohmann::json::parse(pf);
    FmSynth ref;
    for (const auto& [k, v] : preset["layers"][0]["instrument"]["params"].items()) ref.params().set(k, v.get<float>());

    FmSynth m;
    m.loadState({{"syx", rel}, {"voice", 0}});
    CHECK(m.stateError().empty());
    CHECK(m.saveState()["syx"] == rel);
    const Dx7Voice a = FmSynth::paramsToVoice(m.params());
    const Dx7Voice b = FmSynth::paramsToVoice(ref.params());
    CHECK(std::equal(a.data.begin(), a.data.begin() + vced::Name, b.data.begin()));
    CHECK(m.params().get(FmSynth::Alg) == 5.0f);

    // Every factory preset equals its bank voice.
    const char* order[] = {"e-piano/fm-ballad-epiano", "synth-bass/fm-solid-bass", "synth-bass/fm-pop-pluck-bass",
                           "bells-keys/fm-tubular-bells", "bells-keys/fm-marimba", "brass/fm-synth-brass",
                           "synth-lead/fm-harmonica", "bells-keys/fm-clav", "strings/fm-strings",
                           "synth-pad/fm-glass-pad", "organ/fm-drawbar-organ"};
    for (int i = 0; i < 11; ++i) {
        std::ifstream f(paths.root / ("presets/factory/" + std::string(order[i]) + ".json"));
        const auto pj = nlohmann::json::parse(f);
        FmSynth p;
        for (const auto& [k, v] : pj["layers"][0]["instrument"]["params"].items()) p.params().set(k, v.get<float>());
        const Dx7Voice pv = FmSynth::paramsToVoice(p.params());
        INFO("voice " << i << " " << order[i]);
        CHECK(std::equal(pv.data.begin(), pv.data.begin() + vced::Name, bank.voices[static_cast<size_t>(i)].data.begin()));
    }

    // `applied: true` -> the patch params already hold the voice: loadState keeps them.
    FmSynth kept;
    kept.params().set("alg", 17);
    kept.loadState({{"syx", rel}, {"voice", 0}, {"applied", true}});
    CHECK(kept.params().get(FmSynth::Alg) == 17.0f);

    // Out-of-range index clamps; bad paths report an error and leave params alone.
    FmSynth m2;
    m2.loadState({{"syx", rel}, {"voice", 99}});
    CHECK(m2.stateError().empty());
    FmSynth m3;
    m3.loadState({{"syx", "../outside.syx"}});
    CHECK_FALSE(m3.stateError().empty());
    m3.loadState({{"syx", "assets/dx7/missing.syx"}});
    CHECK_FALSE(m3.stateError().empty());
    CHECK(m3.params().get(FmSynth::Alg) == 1.0f);
}

TEST_CASE("fm: sine operator pitch accuracy, tune, transpose, bend", "[fm]") {
    rt::resetViolations();
    for (int note : {33, 45, 57, 69, 81, 93}) {
        FmSynth m; // INIT VOICE: OP1 sine carrier, ratio 1
        m.prepare(kSr, 64);
        const Render r = render(m, {on(0.0, note)}, sec(0.6));
        const double f = measureFreq(r.left, static_cast<size_t>(sec(0.1)), static_cast<size_t>(sec(0.6)));
        INFO("note " << note << " f " << f);
        CHECK(std::fabs(cents(f, noteHz(note))) < 1.0);
    }
    {
        FmSynth m;
        setP(m, "tune", 50.0f);
        setP(m, "transpose", 12);
        m.prepare(kSr, 64);
        const Render r = render(m, {on(0.0, 57)}, sec(0.6));
        const double f = measureFreq(r.left, static_cast<size_t>(sec(0.1)), static_cast<size_t>(sec(0.6)));
        CHECK(std::fabs(cents(f, noteHz(69)) - 50.0) < 1.5);
    }
    {
        FmSynth m;
        setP(m, "pb_range", 2);
        m.prepare(kSr, 64);
        ChannelState cs;
        const Render r = render(m, {on(0.0, 69), {sec(0.0), MidiEvent::pitchBend(1.0f)}}, sec(0.6), 64, &cs);
        const double f = measureFreq(r.left, static_cast<size_t>(sec(0.1)), static_cast<size_t>(sec(0.6)));
        CHECK(std::fabs(cents(f, noteHz(71))) < 2.0);
    }
    {
        // Fixed-frequency mode: coarse 2 (100 Hz), fine 0 -> 100 Hz regardless of the key.
        FmSynth m;
        setP(m, "op1_mode", 1);
        setP(m, "op1_coarse", 2);
        m.prepare(kSr, 64);
        const Render r = render(m, {on(0.0, 90)}, sec(0.6));
        const double f = measureFreq(r.left, static_cast<size_t>(sec(0.1)), static_cast<size_t>(sec(0.6)));
        CHECK(std::fabs(cents(f, 100.0)) < 3.0);
    }
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
}

TEST_CASE("fm: all 32 algorithms x 3 engines render, finite and bounded", "[fm]") {
    rt::resetViolations();
    for (int model = 0; model < 3; ++model) {
        for (int alg = 1; alg <= 32; ++alg) {
            FmSynth m;
            setP(m, "engine_model", static_cast<float>(model));
            setP(m, "alg", static_cast<float>(alg));
            setP(m, "feedback", 7);
            for (int op = 1; op <= 6; ++op) {
                m.params().set(FmSynth::opParam(op, FmSynth::OpOutLevel), 90.0f);
                m.params().set(FmSynth::opParam(op, FmSynth::OpCoarse), static_cast<float>(op % 4));
            }
            m.prepare(kSr, 64);
            const Render r = render(m, {on(0.0, 48), on(0.0, 60), on(0.0, 67), off(0.3, 48), off(0.3, 60),
                                        off(0.3, 67)},
                                    sec(0.6));
            INFO("model " << model << " alg " << alg);
            REQUIRE(allFinite(r.left));
            CHECK(rmsOf(r.left, 0, static_cast<size_t>(sec(0.3))) > 1e-3);
            CHECK(peakOf(r.left) < 8.0f);
            CHECK(r.finalActive == 0); // release 99 -> voices freed
        }
    }
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
}

TEST_CASE("fm: extremes sweep (every param at min and max, random voices)", "[fm]") {
    rt::resetViolations();
    const auto& specs = FmSynth::moduleInfo().params;
    for (size_t i = 0; i < specs.size(); ++i) {
        for (float v : {specs[i].min, specs[i].max}) {
            FmSynth m;
            m.params().set(static_cast<int>(i), v);
            m.prepare(kSr, 64);
            const Render r = render(m, {on(0.0, 21, 1), on(0.0, 108, 127), off(0.1, 21), off(0.1, 108)}, sec(0.15));
            INFO("param " << specs[i].id << " = " << v);
            REQUIRE(allFinite(r.left));
            REQUIRE(peakOf(r.left) < 16.0f);
        }
    }
    std::mt19937 rng(5);
    for (int k = 0; k < 40; ++k) {
        FmSynth m;
        for (size_t i = 0; i < specs.size(); ++i) {
            std::uniform_real_distribution<float> d(specs[i].min, specs[i].max);
            m.params().set(static_cast<int>(i), d(rng));
        }
        m.prepare(kSr, 64);
        std::vector<TimedEvent> evs = {on(0.0, 36, 127), on(0.01, 72, 64), on(0.02, 100, 1), cc(0.05, 1, 127),
                                       {sec(0.06), MidiEvent::pitchBend(-1.0f)}, off(0.2, 36), off(0.2, 72),
                                       off(0.2, 100)};
        const Render r = render(m, evs, sec(0.3), 37);
        INFO("random set " << k);
        REQUIRE(allFinite(r.left));
        REQUIRE(peakOf(r.left) < 16.0f);
    }
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
}

TEST_CASE("fm: polyphony limit and voice stealing", "[fm]") {
    FmSynth m;
    setP(m, "voices", 4);
    m.prepare(kSr, 64);
    std::vector<TimedEvent> evs;
    for (int i = 0; i < 8; ++i) evs.push_back(on(0.05 * i, 60 + i));
    const Render r = render(m, evs, sec(0.6));
    CHECK(r.maxActive == 4);
    CHECK(allFinite(r.left));
    // The newest note sounds: its pitch dominates when alone (others stolen, newest kept).
    FmSynth solo;
    setP(solo, "voices", 1);
    solo.prepare(kSr, 64);
    const Render r1 = render(solo, {on(0.0, 60), on(0.1, 69)}, sec(0.6));
    CHECK(r1.maxActive == 1);
    const double f = measureFreq(r1.left, static_cast<size_t>(sec(0.2)), static_cast<size_t>(sec(0.6)));
    CHECK(std::fabs(cents(f, noteHz(69))) < 2.0);
}

TEST_CASE("fm: sustain pedal holds notes, release frees voices", "[fm]") {
    FmSynth m;
    setP(m, "op1_eg_rate4", 60); // ~0.3 s release
    m.prepare(kSr, 64);
    const Render r = render(m, {on(0.0, 60), cc(0.1, 64, 127), off(0.2, 60), cc(1.0, 64, 0)}, sec(2.0));
    // Held by the pedal between note-off (0.2 s) and pedal-up (1.0 s): level stays up.
    const double held = rmsOf(r.left, static_cast<size_t>(sec(0.7)), static_cast<size_t>(sec(0.95)));
    const double early = rmsOf(r.left, static_cast<size_t>(sec(0.05)), static_cast<size_t>(sec(0.15)));
    CHECK(held > early * 0.7);
    // After pedal-up the note releases and the voice is freed.
    CHECK(rmsOf(r.left, static_cast<size_t>(sec(1.8)), static_cast<size_t>(sec(2.0))) < 1e-5);
    CHECK(r.finalActive == 0);
    // Without the pedal the note is gone by 1 s.
    FmSynth m2;
    setP(m2, "op1_eg_rate4", 60);
    m2.prepare(kSr, 64);
    const Render r2 = render(m2, {on(0.0, 60), off(0.2, 60)}, sec(1.0));
    CHECK(rmsOf(r2.left, static_cast<size_t>(sec(0.8)), static_cast<size_t>(sec(1.0))) < 1e-5);
}

TEST_CASE("fm: block size does not change the output; macros and controllers act", "[fm]") {
    auto run = [](int block, float brightness, float wheel) {
        FmSynth m;
        m.params().set("alg", 1);
        m.params().set("op2_level", 70);
        m.params().set("brightness", brightness);
        m.params().set("lfo_pmd", 0);
        m.params().set("mw_range", 99);
        m.prepare(kSr, 512);
        ChannelState cs;
        cs.modWheel = wheel;
        return render(m, {{1237, MidiEvent::noteOn(60, 100)}, {5003, MidiEvent::noteOn(64, 90)},
                          {24011, MidiEvent::noteOff(60)}, {30001, MidiEvent::noteOff(64)}},
                      sec(0.8), block, &cs)
            .left;
    };
    const auto a = run(64, 0.0f, 0.0f);
    CHECK(a == run(37, 0.0f, 0.0f));
    CHECK(a == run(1, 0.0f, 0.0f));
    CHECK(a == run(512, 0.0f, 0.0f));
    CHECK(run(64, 1.0f, 0.0f) != a); // brightness raises the modulator
    CHECK(run(64, 0.0f, 1.0f) != a); // mod wheel -> LFO pitch
    CHECK(allFinite(run(64, -1.0f, 1.0f)));
}

TEST_CASE("fm: re-striking a sounding note does not dip (envelope restarts from its level)", "[fm]") {
    FmSynth m; // INIT VOICE sine, full sustain
    m.prepare(kSr, 64);
    const Render r = render(m, {on(0.0, 69), cc(0.05, 64, 127), off(0.1, 69), on(0.3, 69), on(0.5, 69)}, sec(0.7));
    const double steady = rmsOf(r.left, static_cast<size_t>(sec(0.2)), static_cast<size_t>(sec(0.29)));
    double minWin = 1e9;
    for (int64_t s0 = sec(0.29); s0 < sec(0.6); s0 += 64)
        minWin = std::min(minWin, rmsOf(r.left, static_cast<size_t>(s0), static_cast<size_t>(s0 + 64)));
    CHECK(minWin > steady * 0.7);
    CHECK(r.maxActive == 1);
}

TEST_CASE("fm: mono / legato, sostenuto, op switch, aftertouch, sample rates", "[fm]") {
    rt::resetViolations();
    for (int mode : {1, 2}) {
        FmSynth m;
        setP(m, "voice_mode", static_cast<float>(mode));
        m.prepare(kSr, 64);
        const Render r = render(m, {on(0.0, 60), on(0.2, 69), off(0.5, 69), off(0.6, 60)}, sec(1.0));
        INFO("mode " << mode);
        CHECK(r.maxActive == 1);
        const double f = measureFreq(r.left, static_cast<size_t>(sec(0.3)), static_cast<size_t>(sec(0.5)));
        CHECK(std::fabs(cents(f, noteHz(69))) < 2.0);
        const double back = measureFreq(r.left, static_cast<size_t>(sec(0.52)), static_cast<size_t>(sec(0.6)));
        CHECK(std::fabs(cents(back, noteHz(60))) < 3.0); // returns to the held note
        CHECK(allFinite(r.left));
    }
    {
        // Sostenuto latches only notes held when it went down.
        FmSynth m;
        setP(m, "op1_eg_rate4", 70);
        m.prepare(kSr, 64);
        const Render r = render(m, {on(0.0, 60), cc(0.05, 66, 127), on(0.1, 72), off(0.2, 60), off(0.2, 72)},
                                sec(0.8));
        CHECK(rmsOf(r.left, static_cast<size_t>(sec(0.6)), static_cast<size_t>(sec(0.8))) > 1e-3);
        const double f = measureFreq(r.left, static_cast<size_t>(sec(0.5)), static_cast<size_t>(sec(0.8)));
        CHECK(std::fabs(cents(f, noteHz(60))) < 2.0);
    }
    {
        FmSynth m;
        setP(m, "op1_on", 0);
        m.prepare(kSr, 64);
        const Render r = render(m, {on(0.0, 60)}, sec(0.2));
        CHECK(peakOf(r.left) < 1e-4f);
    }
    {
        // Aftertouch -> amp (with AMS) changes the level; no aftertouch routing -> identical.
        auto run = [](bool route) {
            FmSynth m;
            m.params().set("op1_ams", 3);
            m.params().set("at_range", 99);
            m.params().set("at_amp", route ? 1.0f : 0.0f);
            m.prepare(kSr, 64);
            ChannelState cs;
            cs.aftertouch = 1.0f;
            return render(m, {on(0.0, 60)}, sec(0.4), 64, &cs).left;
        };
        const auto routed = run(true), plain = run(false);
        CHECK(rmsOf(routed, 0, routed.size()) < 0.9 * rmsOf(plain, 0, plain.size()));
    }
    for (double sr : {44100.0, 96000.0, 48000.0}) {
        FmSynth m;
        m.prepare(sr, 64);
        const auto n = static_cast<int64_t>(0.5 * sr);
        const Render r = render(m, {{0, MidiEvent::noteOn(81, 100)}}, n, 64, nullptr, sr);
        const double f = measureFreq(r.left, static_cast<size_t>(n / 5), static_cast<size_t>(n), sr);
        INFO("sr " << sr);
        CHECK(std::fabs(cents(f, noteHz(81))) < 1.0);
    }
    if (rt::checksEnabled()) CHECK(rt::violationCount() == 0);
}
