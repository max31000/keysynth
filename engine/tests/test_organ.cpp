// Tests for `organ` (tonewheel), `combo` (transistor combo organ) and `rotary` (Leslie) modules.

#include "core/ChannelState.h"
#include "core/RtCheck.h"
#include "effects/rotary/RotaryFx.h"
#include "instruments/combo/ComboOrgan.h"
#include "instruments/organ/ToneWheelOrgan.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <vector>

using namespace ks;

namespace {

constexpr double kSr = 48000.0;

struct Timed {
    int64_t at; // sample
    MidiEvent e;
};

// Renders `total` samples of a module in blocks; events are delivered at their sample position.
// For effects, `input` (if set) fills the block before processing.
struct Rendered {
    std::vector<float> l, r;
};
Rendered renderModule(Module& m, std::vector<Timed> ev, int64_t total, int block = 64, ChannelState* cs = nullptr,
                      const std::function<void(int64_t, float&, float&)>& input = {},
                      const std::function<void(int64_t)>& perBlock = {}) {
    std::stable_sort(ev.begin(), ev.end(), [](const Timed& a, const Timed& b) { return a.at < b.at; });
    Rendered out;
    out.l.assign(static_cast<size_t>(total), 0.0f);
    out.r.assign(static_cast<size_t>(total), 0.0f);
    ChannelState local;
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.channel = cs ? cs : &local;
    std::vector<MidiEvent> blockEv;
    blockEv.reserve(256);
    size_t ei = 0;
    for (int64_t pos = 0; pos < total; pos += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, total - pos));
        if (perBlock) perBlock(pos);
        blockEv.clear();
        while (ei < ev.size() && ev[ei].at < pos + n) {
            MidiEvent e = ev[ei].e;
            e.sampleOffset = static_cast<uint32_t>(std::max<int64_t>(0, ev[ei].at - pos));
            blockEv.push_back(e);
            ++ei;
        }
        AudioBlock b{out.l.data() + pos, out.r.data() + pos, n};
        if (input)
            for (int i = 0; i < n; ++i) input(pos + i, b.left[i], b.right[i]);
        ctx.numSamples = n;
        ctx.sampleTime = pos;
        {
            rt::RtScope scope; // allocations/locks inside process are counted as RT violations
            m.process(b, MidiEventSpan(blockEv.data(), blockEv.size()), ctx);
        }
    }
    return out;
}

int64_t sec(double s) { return static_cast<int64_t>(s * kSr); }

// Amplitude of the component at `hz` in x[from, to) (Hann-windowed single DFT bin).
double amplitudeAt(const std::vector<float>& x, double hz, int64_t from, int64_t to) {
    std::complex<double> acc = 0.0;
    double wsum = 0.0;
    const double n = static_cast<double>(to - from);
    for (int64_t i = from; i < to; ++i) {
        const double t = static_cast<double>(i - from);
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * t / n);
        acc += w * static_cast<double>(x[static_cast<size_t>(i)]) *
               std::polar(1.0, -2.0 * 3.14159265358979 * hz * static_cast<double>(i) / kSr);
        wsum += w;
    }
    return 2.0 * std::abs(acc) / wsum;
}

double rms(const std::vector<float>& x, int64_t from, int64_t to) {
    double s = 0.0;
    for (int64_t i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return std::sqrt(s / static_cast<double>(std::max<int64_t>(1, to - from)));
}

// Relative peak frequency deviation from positive zero crossings, frequency averaged over `avg` periods.
double pitchDeviation(const std::vector<float>& x, int64_t from, int64_t to, int avg = 4) {
    std::vector<double> zc;
    for (int64_t i = from + 1; i < to; ++i) {
        const float a = x[static_cast<size_t>(i - 1)], b = x[static_cast<size_t>(i)];
        if (a < 0.0f && b >= 0.0f) zc.push_back(static_cast<double>(i - 1) + a / (a - b));
    }
    std::vector<double> f;
    for (size_t k = 0; k + static_cast<size_t>(avg) < zc.size(); ++k)
        f.push_back(avg / (zc[k + static_cast<size_t>(avg)] - zc[k]));
    if (f.size() < 4) return 0.0;
    const auto [mn, mx] = std::minmax_element(f.begin(), f.end());
    double mean = 0.0;
    for (double v : f) mean += v;
    mean /= static_cast<double>(f.size());
    return (*mx - *mn) / (2.0 * mean);
}

void setOrganQuiet(ToneWheelOrgan& o) {
    auto& p = o.params();
    for (int b = 0; b < ToneWheelOrgan::kBars; ++b) p.set(b, 0.0f);
    p.set(ToneWheelOrgan::Perc, 0.0f);
    p.set(ToneWheelOrgan::Vibrato, 0.0f);
    p.set(ToneWheelOrgan::Click, 0.0f);
    p.set(ToneWheelOrgan::Leakage, 0.0f);
    p.set(ToneWheelOrgan::Drive, 0.0f);
    p.set(ToneWheelOrgan::VolumeDb, 0.0f);
}

bool allFinite(const Rendered& r, float bound = 20.0f) {
    for (size_t i = 0; i < r.l.size(); ++i)
        if (!std::isfinite(r.l[i]) || !std::isfinite(r.r[i]) || std::fabs(r.l[i]) > bound || std::fabs(r.r[i]) > bound)
            return false;
    return true;
}

} // namespace

TEST_CASE("Organ: tonewheel frequencies follow the B-3 gear ratios", "[organ]") {
    REQUIRE(ToneWheelOrgan::wheelFrequency(46) == 440.0); // A4: 20 * 88/64 * 16
    REQUIRE(std::fabs(ToneWheelOrgan::wheelFrequency(1) - 32.692) < 0.01);
    REQUIRE(std::fabs(ToneWheelOrgan::wheelFrequency(85) - 4189.09) < 0.05); // 192 teeth on the F gear
    for (int w = 2; w <= ToneWheelOrgan::kWheels; ++w) {
        INFO("wheel " << w);
        const double f = ToneWheelOrgan::wheelFrequency(w), fp = ToneWheelOrgan::wheelFrequency(w - 1);
        REQUIRE(f > fp);
        const double cents = 1200.0 * std::log2(f / fp);
        REQUIRE(std::fabs(cents - 100.0) < 3.0); // near-ET, not exact ET
    }
    // Not exactly equal-tempered: C#4 wheel deviates measurably from ET.
    const double cs = ToneWheelOrgan::wheelFrequency(13 + 25);
    REQUIRE(std::fabs(cs - 277.1826) > 0.005);
    REQUIRE(ToneWheelOrgan::drawbarGain(8) == 1.0f);
    REQUIRE(std::fabs(ToneWheelOrgan::drawbarGain(6) - 0.5f) < 1e-6f);
    REQUIRE(ToneWheelOrgan::drawbarGain(0) == 0.0f);
}

TEST_CASE("Organ: each drawbar produces its partial (single key)", "[organ]") {
    const double ratio[ToneWheelOrgan::kBars] = {0.5, 1.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
    for (int bar = 0; bar < ToneWheelOrgan::kBars; ++bar) {
        INFO("drawbar " << bar);
        ToneWheelOrgan o;
        o.prepare(kSr, 64);
        setOrganQuiet(o);
        o.params().set(bar, 8.0f);
        const auto r = renderModule(o, {{0, MidiEvent::noteOn(60, 100)}}, sec(0.6));
        const double fTarget = ToneWheelOrgan::wheelFrequency(ToneWheelOrgan::wheelFor(60, bar));
        // The footage is the expected (tempered) harmonic of C4, within the Hammond's tuning error.
        const double et = 261.6256 * std::exp2(std::round(12.0 * std::log2(ratio[bar])) / 12.0);
        REQUIRE(std::fabs(1200.0 * std::log2(fTarget / et)) < 3.0);
        const double a = amplitudeAt(r.l, fTarget, sec(0.1), sec(0.6));
        CAPTURE(a);
        REQUIRE(a > 0.08);
        REQUIRE(a < 0.2);
        for (int other = 0; other < ToneWheelOrgan::kBars; ++other) {
            if (other == bar) continue;
            const double fo = ToneWheelOrgan::wheelFrequency(ToneWheelOrgan::wheelFor(60, other));
            INFO("other " << other);
            REQUIRE(amplitudeAt(r.l, fo, sec(0.1), sec(0.6)) < a * 0.03);
        }
    }
    // Drawbar position 6 is ~6 dB below position 8.
    ToneWheelOrgan o8, o6;
    for (auto* o : {&o8, &o6}) {
        o->prepare(kSr, 64);
        setOrganQuiet(*o);
    }
    o8.params().set(ToneWheelOrgan::Db8, 8.0f);
    o6.params().set(ToneWheelOrgan::Db8, 6.0f);
    const double f = ToneWheelOrgan::wheelFrequency(ToneWheelOrgan::wheelFor(60, ToneWheelOrgan::Db8));
    const double a8 = amplitudeAt(renderModule(o8, {{0, MidiEvent::noteOn(60, 100)}}, sec(0.5)).l, f, sec(0.1), sec(0.5));
    const double a6 = amplitudeAt(renderModule(o6, {{0, MidiEvent::noteOn(60, 100)}}, sec(0.5)).l, f, sec(0.1), sec(0.5));
    REQUIRE(std::fabs(20.0 * std::log10(a6 / a8) + 6.0) < 0.5);
}

TEST_CASE("Organ: manual foldback at the bottom (16') and top (high footages)", "[organ]") {
    using O = ToneWheelOrgan;
    // Bottom octave of the 16' repeats the next octave (wheels 1-12 are pedal-only).
    REQUIRE(O::wheelFor(36, O::Db16) == 13);
    REQUIRE(O::wheelFor(47, O::Db16) == 24);
    REQUIRE(O::wheelFor(48, O::Db16) == 13);
    REQUIRE(O::wheelFor(36, O::Db8) == 13);
    // Top: the 1' of F#5 (MIDI 78) is wheel 13+42+36 = 91, the last wheel; above it the 1' folds back.
    REQUIRE(O::wheelFor(78, O::Db1) == 91);
    REQUIRE(O::wheelFor(79, O::Db1) == 80);
    REQUIRE(O::wheelFor(84, O::Db1) == 85);
    for (int n = 36; n <= 96; ++n)
        for (int b = 0; b < O::kBars; ++b) {
            const int w = O::wheelFor(n, b);
            REQUIRE(w >= 1);
            REQUIRE(w <= O::kWheels);
        }
    REQUIRE(O::wheelFor(96, O::Db1) == 85);
    REQUIRE(O::wheelFor(96, O::Db2) == 85);
    REQUIRE(O::wheelFor(96, O::Db4) == 85);
    // Out-of-manual MIDI notes fold into C2..C7; releasing one folded duplicate keeps the key held.
    REQUIRE(O::wheelFor(24, O::Db8) == O::wheelFor(36, O::Db8));
    {
        ToneWheelOrgan od;
        od.prepare(kSr, 64);
        setOrganQuiet(od);
        od.params().set(O::Db8, 8.0f);
        const auto rd = renderModule(od, {{0, MidiEvent::noteOn(36, 100)}, {0, MidiEvent::noteOn(24, 100)},
                                          {sec(0.1), MidiEvent::noteOff(24)}}, sec(0.4));
        REQUIRE(amplitudeAt(rd.l, O::wheelFrequency(13), sec(0.2), sec(0.4)) > 0.05);
    }
    // Audio: top key C7 with only the 1' sounds C8 (folded), not C9.
    ToneWheelOrgan o;
    o.prepare(kSr, 64);
    setOrganQuiet(o);
    o.params().set(O::Db1, 8.0f);
    const auto r = renderModule(o, {{0, MidiEvent::noteOn(96, 100)}}, sec(0.4));
    const double folded = amplitudeAt(r.l, O::wheelFrequency(85), sec(0.1), sec(0.4));
    const double unfolded = amplitudeAt(r.l, 8372.0, sec(0.1), sec(0.4));
    REQUIRE(folded > 0.05);
    REQUIRE(unfolded < folded * 0.01);
    // Audio: bottom key C2 with only the 16' sounds C2 (folded), not C1.
    ToneWheelOrgan o2;
    o2.prepare(kSr, 64);
    setOrganQuiet(o2);
    o2.params().set(O::Db16, 8.0f);
    const auto r2 = renderModule(o2, {{0, MidiEvent::noteOn(36, 100)}}, sec(0.6));
    REQUIRE(amplitudeAt(r2.l, O::wheelFrequency(13), sec(0.1), sec(0.6)) > 0.05);
    REQUIRE(amplitudeAt(r2.l, O::wheelFrequency(1), sec(0.1), sec(0.6)) < 0.002);
}

TEST_CASE("Organ: percussion is single-trigger and cancels the 1' drawbar", "[organ]") {
    using O = ToneWheelOrgan;
    O o;
    o.prepare(kSr, 64);
    setOrganQuiet(o);
    o.params().set(O::Perc, 1.0f);
    o.params().set(O::PercHarmonic, 0.0f); // 2nd = 4'
    o.params().set(O::PercDecay, 0.0f);    // fast
    o.params().set(O::Db1, 8.0f);          // cancelled while percussion is on
    std::vector<Timed> ev = {
        {0, MidiEvent::noteOn(60, 100)},
        {sec(0.4), MidiEvent::noteOn(67, 100)}, // legato: C4 still held -> no retrigger
        {sec(0.8), MidiEvent::noteOff(60)},
        {sec(0.8), MidiEvent::noteOff(67)},
        {sec(1.0), MidiEvent::noteOn(64, 100)}, // detached: retrigger
    };
    int triggersMid = -1;
    const auto r = renderModule(o, ev, sec(1.3), 64, nullptr, {}, [&](int64_t pos) {
        if (pos == sec(0.6) / 64 * 64) triggersMid = o.percussionTriggers();
    });
    REQUIRE(triggersMid == 1); // after the legato G4
    REQUIRE(o.percussionTriggers() == 2);
    const double fC = O::wheelFrequency(O::wheelFor(60, O::Db4));
    const double fG = O::wheelFrequency(O::wheelFor(67, O::Db4));
    const double fE = O::wheelFrequency(O::wheelFor(64, O::Db4));
    const double first = amplitudeAt(r.l, fC, 0, sec(0.05));
    const double legato = amplitudeAt(r.l, fG, sec(0.4), sec(0.45));
    const double second = amplitudeAt(r.l, fE, sec(1.0), sec(1.05));
    CAPTURE(first, legato, second);
    REQUIRE(first > 0.03);
    REQUIRE(legato < first * 0.15);
    REQUIRE(second > first * 0.7);
    // Percussion decays (fast: time constant ~0.15 s -> about -20 dB around 0.34 s).
    const double later = amplitudeAt(r.l, fC, sec(0.3), sec(0.38));
    REQUIRE(later < first * 0.16);
    REQUIRE(later > first * 0.05);
    // 1' drawbar cancelled.
    REQUIRE(amplitudeAt(r.l, O::wheelFrequency(O::wheelFor(60, O::Db1)), sec(0.05), sec(0.35)) < 0.002);
}

TEST_CASE("Organ: scanner vibrato depth order V1 < V2 < V3, chorus C1 < C3", "[organ]") {
    using O = ToneWheelOrgan;
    double dev[7] = {};
    for (int mode = 0; mode <= 6; ++mode) {
        O o;
        o.prepare(kSr, 64);
        setOrganQuiet(o);
        o.params().set(O::Db8, 8.0f);
        o.params().set(O::Vibrato, static_cast<float>(mode));
        o.reset();
        const auto r = renderModule(o, {{0, MidiEvent::noteOn(57, 100)}}, sec(1.5)); // A3 = 220 Hz
        dev[mode] = pitchDeviation(r.l, sec(0.2), sec(1.5));
    }
    CAPTURE(dev[0], dev[1], dev[2], dev[3], dev[4], dev[5], dev[6]);
    REQUIRE(dev[0] < 0.0005);
    REQUIRE(dev[1] > dev[0] * 2.0);
    REQUIRE(dev[2] > dev[1] * 1.2);
    REQUIRE(dev[3] > dev[2] * 1.2);
    REQUIRE(dev[3] > 0.004); // V3: roughly +-1.4 % (+-24 cents)
    REQUIRE(dev[3] < 0.03);
    REQUIRE(dev[6] > dev[4]);
}

TEST_CASE("Rotary: rotor speeds ramp with horn faster than drum", "[organ][rotary]") {
    RotaryFx fx;
    fx.prepare(kSr, 64);
    auto& p = fx.params();
    p.set(RotaryFx::Speed, RotaryFx::Slow);
    fx.reset();
    const float hornSlow = p.get(RotaryFx::HornSlowRpm), hornFast = p.get(RotaryFx::HornFastRpm);
    const float drumSlow = p.get(RotaryFx::DrumSlowRpm), drumFast = p.get(RotaryFx::DrumFastRpm);
    auto run = [&](double s) { renderModule(fx, {}, sec(s)); };
    run(0.5);
    REQUIRE(std::fabs(p.get(RotaryFx::HornRpm) - hornSlow) < 0.5f);
    REQUIRE(std::fabs(p.get(RotaryFx::DrumRpm) - drumSlow) < 0.5f);

    p.set(RotaryFx::Speed, RotaryFx::Fast);
    run(0.5);
    const float h05 = p.get(RotaryFx::HornRpm), d05 = p.get(RotaryFx::DrumRpm);
    CAPTURE(h05, d05);
    REQUIRE(h05 > hornSlow + 0.9f * (hornFast - hornSlow)); // horn: ~0.16 s time constant
    REQUIRE(d05 < drumSlow + 0.25f * (drumFast - drumSlow)); // drum: heavy, ~4 s
    REQUIRE(d05 > drumSlow + 0.05f * (drumFast - drumSlow));
    run(11.5);
    REQUIRE(p.get(RotaryFx::DrumRpm) > drumSlow + 0.9f * (drumFast - drumSlow));

    p.set(RotaryFx::Speed, RotaryFx::Slow);
    run(1.5);
    const float h15 = p.get(RotaryFx::HornRpm), d15 = p.get(RotaryFx::DrumRpm);
    CAPTURE(h15, d15);
    REQUIRE(h15 < hornSlow + 0.02f * (hornFast - hornSlow));
    REQUIRE(d15 > drumSlow + 0.15f * (drumFast - drumSlow)); // drum coasts down over seconds
    REQUIRE(d15 < drumSlow + 0.5f * (drumFast - drumSlow));

    p.set(RotaryFx::Speed, RotaryFx::Stop);
    run(10.0);
    REQUIRE(p.get(RotaryFx::HornRpm) < 0.5f);
    REQUIRE(p.get(RotaryFx::DrumRpm) < 0.5f);

    // Mod wheel >= 64 = fast (option on by default); sustain toggling is off by default.
    p.set(RotaryFx::Speed, RotaryFx::Slow);
    ChannelState cs;
    cs.modWheel = 1.0f;
    renderModule(fx, {}, sec(1.0), 64, &cs);
    REQUIRE(p.get(RotaryFx::HornRpm) > 0.9f * hornFast);
    cs.modWheel = 0.0f;
    cs.sustain = true;
    renderModule(fx, {}, sec(2.0), 64, &cs);
    REQUIRE(p.get(RotaryFx::HornRpm) < hornSlow + 5.0f);
    // Sustain toggle option: a pedal press toggles fast.
    p.set(RotaryFx::SustainToggle, 1.0f);
    cs.sustain = false;
    renderModule(fx, {}, sec(0.1), 64, &cs);
    cs.sustain = true;
    renderModule(fx, {}, sec(1.0), 64, &cs);
    REQUIRE(p.get(RotaryFx::HornRpm) > 0.9f * hornFast);
}

TEST_CASE("Rotary: Doppler pitch modulation and stereo AM", "[organ][rotary]") {
    RotaryFx fx;
    fx.prepare(kSr, 64);
    fx.params().set(RotaryFx::Speed, RotaryFx::Fast);
    fx.params().set(RotaryFx::Drive, 0.0f);
    fx.reset();
    auto sine = [](double hz) {
        return [hz](int64_t i, float& l, float& r) {
            l = r = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / kSr));
        };
    };
    const auto hi = renderModule(fx, {}, sec(2.0), 64, nullptr, sine(3000.0));
    REQUIRE(allFinite(hi, 2.0f));
    const double dev = pitchDeviation(hi.l, sec(0.5), sec(2.0), 16);
    CAPTURE(dev);
    REQUIRE(dev > 0.005); // horn Doppler at ~6.7 Hz: ~+-1.8 %
    REQUIRE(dev < 0.05);
    // L and R differ (mic spread).
    double diff = 0.0;
    for (int64_t i = sec(0.5); i < sec(2.0); ++i) diff += std::fabs(hi.l[static_cast<size_t>(i)] - hi.r[static_cast<size_t>(i)]);
    REQUIRE(diff / static_cast<double>(sec(1.5)) > 0.01);
    // Silence in -> silence out (no self-noise), so tails decay.
    fx.reset();
    const auto quiet = renderModule(fx, {}, sec(0.5));
    REQUIRE(rms(quiet.l, 0, sec(0.5)) == 0.0);
}

TEST_CASE("Combo: divide-down voicings, bass section, expression", "[organ][combo]") {
    using C = ComboOrgan;
    auto make = [](C& c) {
        c.prepare(kSr, 64);
        auto& p = c.params();
        p.set(C::Voicing, 0.0f);
        for (int i = C::Vox16; i <= C::VoxReed; ++i) p.set(i, 0.0f);
        p.set(C::VibratoOn, 0.0f);
        p.set(C::Leakage, 0.0f);
        p.set(C::Bass, 0.0f);
        c.reset();
    };
    const double a4 = 440.0 * std::exp2(C::masterDetuneCents(9) / 1200.0);
    {
        // Vox 8' flute only: a sine at the (slightly mistuned) master pitch.
        C c;
        make(c);
        c.params().set(C::Vox8, 8.0f);
        c.params().set(C::VoxFlute, 8.0f);
        const auto r = renderModule(c, {{0, MidiEvent::noteOn(69, 100)}}, sec(0.5));
        const double f1 = amplitudeAt(r.l, a4, sec(0.1), sec(0.5));
        REQUIRE(f1 > 0.03);
        REQUIRE(amplitudeAt(r.l, 2 * a4, sec(0.1), sec(0.5)) < f1 * 0.02);
        REQUIRE(amplitudeAt(r.l, 3 * a4, sec(0.1), sec(0.5)) < f1 * 0.02);
    }
    {
        // Vox reed: divider staircase has even and odd harmonics (ramp-ish).
        C c;
        make(c);
        c.params().set(C::Vox8, 8.0f);
        c.params().set(C::VoxReed, 8.0f);
        const auto r = renderModule(c, {{0, MidiEvent::noteOn(57, 100)}}, sec(0.5));
        const double h1 = amplitudeAt(r.l, a4 / 2, sec(0.1), sec(0.5));
        const double h2 = amplitudeAt(r.l, a4, sec(0.1), sec(0.5));
        const double h3 = amplitudeAt(r.l, 1.5 * a4, sec(0.1), sec(0.5));
        CAPTURE(h1, h2, h3);
        REQUIRE(h2 > h1 * 0.25);
        REQUIRE(h3 > h1 * 0.15);
    }
    {
        // Farfisa with bass section: a key below the split plays only the bass voice (16' + 8', low-passed);
        // the strings tab must not sound there. Compare the >1.5 kHz energy with the strings on vs off.
        auto renderBass = [&](bool strings, bool bass, int note) {
            C c;
            make(c);
            c.params().set(C::Voicing, 1.0f);
            c.params().set(C::FarFlute8, 0.0f);
            c.params().set(C::FarStrings8, strings ? 1.0f : 0.0f);
            c.params().set(C::Bass, bass ? 1.0f : 0.0f);
            c.params().set(C::BassSplit, 48.0f);
            return renderModule(c, {{0, MidiEvent::noteOn(note, 100)}}, sec(0.6)).l;
        };
        auto highEnergy = [&](const std::vector<float>& x) {
            double e = 0.0;
            for (int h = 14; h <= 40; ++h) e += amplitudeAt(x, h * a4 / 4, sec(0.1), sec(0.6)); // A2 harmonics > 1.5 kHz
            return e;
        };
        const auto withStrings = renderBass(true, true, 45);  // A2, below split
        const auto noStrings = renderBass(false, true, 45);
        const double sub = amplitudeAt(withStrings, a4 / 8, sec(0.1), sec(0.6)); // bass 16'
        CAPTURE(sub, highEnergy(withStrings), highEnergy(noStrings));
        REQUIRE(sub > 0.01);
        REQUIRE(std::fabs(highEnergy(withStrings) - highEnergy(noStrings)) < 1e-6 + 0.01 * highEnergy(noStrings));
        // With the bass section off the same key plays the strings.
        REQUIRE(highEnergy(renderBass(true, false, 45)) > 20.0 * highEnergy(noStrings) + 1e-4);
    }
    {
        // Expression (CC11) acts as the swell pedal.
        C c1, c2;
        make(c1);
        make(c2);
        for (C* c : {&c1, &c2}) {
            c->params().set(C::Vox8, 8.0f);
            c->params().set(C::VoxReed, 8.0f);
        }
        ChannelState half;
        half.expression = 0.5f;
        const auto full = renderModule(c1, {{0, MidiEvent::noteOn(60, 100)}}, sec(0.5));
        const auto soft = renderModule(c2, {{0, MidiEvent::noteOn(60, 100)}}, sec(0.5), 64, &half);
        const double dB = 20.0 * std::log10(rms(soft.l, sec(0.2), sec(0.5)) / rms(full.l, sec(0.2), sec(0.5)));
        REQUIRE(dB < -10.0);
        REQUIRE(dB > -20.0);
    }
}

TEST_CASE("Organ/combo/rotary: no NaN/Inf over param extremes, sample rates, block sizes", "[organ]") {
    std::vector<Timed> ev;
    for (int n = 24; n <= 108; n += 7) ev.push_back({0, MidiEvent::noteOn(n, 127)});
    for (int n = 24; n <= 108; n += 3) ev.push_back({sec(0.05) + n * 40, MidiEvent::noteOn(n, 1)});
    ev.push_back({sec(0.2), MidiEvent::allNotesOff()});
    auto sweep = [&](Module& m, bool effect) {
        const int count = m.params().size();
        for (int mode = 0; mode < 3; ++mode) {
            for (int i = 0; i < count; ++i) {
                const ParamSpec& s = m.params().spec(i);
                if (s.isReadOnly()) continue;
                const float v = mode == 0 ? s.min : mode == 1 ? s.max : (i % 2 ? s.min : s.max);
                m.params().set(i, v);
            }
            for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
                for (int block : {1, 33, 512}) {
                    INFO(m.info().typeId << " mode " << mode << " sr " << sr << " block " << block);
                    m.prepare(sr, block);
                    m.reset();
                    // Non-default controllers on odd modes: mod wheel up, sustain down, swell closed.
                    ChannelState cs;
                    if (mode == 1) {
                        cs.modWheel = 1.0f;
                        cs.sustain = true;
                        cs.expression = 0.0f;
                    }
                    const auto r = renderModule(m, ev, sec(0.3), block, &cs,
                                                effect ? std::function<void(int64_t, float&, float&)>(
                                                             [](int64_t i, float& l, float& rr) {
                                                                 l = (i % 97) < 48 ? 0.9f : -0.9f;
                                                                 rr = -l;
                                                             })
                                                       : std::function<void(int64_t, float&, float&)>());
                    REQUIRE(allFinite(r));
                }
            }
        }
    };
    ToneWheelOrgan o;
    ComboOrgan c;
    RotaryFx fx;
    rt::resetViolations();
    sweep(o, false);
    sweep(c, false);
    sweep(fx, true);
    if (rt::checksEnabled()) REQUIRE(rt::violationCount() == 0);
}

TEST_CASE("Organ: CPU for a full 10-note chord, all drawbars, glissando", "[organ][bench]") {
    using O = ToneWheelOrgan;
    O o;
    o.prepare(kSr, 64);
    for (int b = 0; b < O::kBars; ++b) o.params().set(b, 8.0f);
    o.params().set(O::Perc, 1.0f);
    o.params().set(O::Vibrato, 6.0f);
    o.params().set(O::Drive, 0.5f);
    o.params().set(O::Click, 1.0f);
    o.params().set(O::Leakage, 1.0f);
    std::vector<Timed> ev;
    const int chord[10] = {36, 43, 48, 52, 55, 60, 64, 67, 72, 76};
    for (int n : chord) ev.push_back({0, MidiEvent::noteOn(n, 100)});
    for (int k = 0; k < 61; ++k) { // glissando over the whole manual every second
        for (int rep = 0; rep < 2; ++rep) {
            const int64_t t = sec(0.5 + 1.0 * rep) + k * 400;
            ev.push_back({t, MidiEvent::noteOn(36 + k, 100)});
            ev.push_back({t + 1200, MidiEvent::noteOff(36 + k)});
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = renderModule(o, ev, sec(3.0));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double factor = 3.0 / wall;
    std::printf("organ 10-note chord + glissando, all drawbars: realtime factor %.1fx\n", factor);
    REQUIRE(allFinite(r));
    REQUIRE(factor > 10.0); // Release measures ~400x; generous margin for Debug/loaded machines
}
