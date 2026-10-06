// Effects: chorus, ensemble, phaser, flanger, delay, reverb, drive, compressor, eq (+ shared dsp primitives).
// Measurements are made on the module directly (no Engine) at 48 kHz unless noted.

#include "core/ModuleRegistry.h"
#include "core/RtCheck.h"
#include "dsp/Denormals.h"
#include "dsp/HalfbandIir.h"
#include "dsp/NoteDivision.h"
#include "effects/chorus/ChorusFx.h"
#include "effects/compressor/CompressorFx.h"
#include "effects/delay/DelayFx.h"
#include "effects/drive/DriveFx.h"
#include "effects/ensemble/EnsembleFx.h"
#include "effects/eq/EqFx.h"
#include "effects/flanger/FlangerFx.h"
#include "effects/phaser/PhaserFx.h"
#include "effects/reverb/ReverbFx.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace ks;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 64;
const char* const kFxTypes[] = {"chorus", "ensemble", "phaser", "flanger", "delay",
                                "reverb", "drive",    "compressor", "eq"};

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    size_t size() const { return l.size(); }
};

std::unique_ptr<Module> make(const std::string& type, std::initializer_list<std::pair<const char*, float>> ps = {},
                             double sr = kSr, int block = kBlock) {
    auto m = defaultRegistry().create(type);
    REQUIRE(m);
    for (const auto& [id, v] : ps) REQUIRE(m->params().set(id, v));
    m->prepare(sr, block);
    return m;
}

void setP(Module& m, const char* id, float v) { REQUIRE(m.params().set(id, v)); }

// Process in place in blocks; tempo for synced effects.
void run(Module& m, Stereo& s, double tempo = 120.0, int block = kBlock, double sr = kSr) {
    static const ChannelState cs{};
    ProcessContext ctx;
    ctx.sampleRate = sr;
    ctx.channel = &cs;
    ctx.transport.tempo = tempo;
    for (size_t off = 0; off < s.size(); off += static_cast<size_t>(block)) {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), s.size() - off));
        AudioBlock b{s.l.data() + off, s.r.data() + off, n};
        ctx.numSamples = n;
        m.process(b, {}, ctx);
        ctx.sampleTime += n;
    }
}

Stereo sine(double hz, double amp, size_t n, double sr = kSr) {
    Stereo s(n);
    for (size_t i = 0; i < n; ++i) {
        const float v = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / sr));
        s.l[i] = s.r[i] = v;
    }
    return s;
}

Stereo noise(size_t n, float amp, uint32_t seed = 1) {
    Stereo s(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s.l[i] = amp * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
        x = x * 1664525u + 1013904223u;
        s.r[i] = amp * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
    }
    return s;
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
    double s = 0.0;
    b = std::min(b, v.size());
    for (size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
    return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}
double peakAbs(const std::vector<float>& v, size_t a = 0, size_t b = SIZE_MAX) {
    double p = 0.0;
    b = std::min(b, v.size());
    for (size_t i = a; i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
    return p;
}
bool allFinite(const Stereo& s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
    return true;
}
double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// Radix-2 FFT (n power of two), in place.
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
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
// Magnitude spectrum (dB) of x[a..a+n), zero padded, optional Hann window.
std::vector<double> spectrumDb(const std::vector<float>& x, size_t a, size_t n, bool hann) {
    std::vector<std::complex<double>> c(n);
    for (size_t i = 0; i < n; ++i) {
        const double w = hann ? 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) / static_cast<double>(n)) : 1.0;
        c[i] = (a + i < x.size() ? x[a + i] : 0.0f) * w;
    }
    fft(c);
    std::vector<double> m(n / 2);
    for (size_t i = 0; i < n / 2; ++i) m[i] = db(std::abs(c[i]));
    return m;
}

// Index of the max |x| in [a, b).
size_t argmaxAbs(const std::vector<float>& x, size_t a, size_t b) {
    size_t k = a;
    for (size_t i = a; i < std::min(b, x.size()); ++i)
        if (std::fabs(x[i]) > std::fabs(x[k])) k = i;
    return k;
}

// Schroeder backward-integrated RT60 from the -5..-35 dB range (T30 x 2).
double rt60(const std::vector<float>& h, double sr) {
    std::vector<double> e(h.size());
    double acc = 0.0;
    for (size_t i = h.size(); i-- > 0;) {
        acc += static_cast<double>(h[i]) * h[i];
        e[i] = acc;
    }
    const double e0 = e[0];
    size_t i5 = 0, i35 = 0;
    for (size_t i = 0; i < e.size(); ++i) {
        const double d = 10.0 * std::log10(std::max(e[i] / e0, 1e-30));
        if (!i5 && d <= -5.0) i5 = i;
        if (!i35 && d <= -35.0) {
            i35 = i;
            break;
        }
    }
    if (!i5 || !i35) return -1.0;
    return 2.0 * static_cast<double>(i35 - i5) / sr;
}

} // namespace

// --------------------------------------------------------------------------------------------- shared dsp

TEST_CASE("Halfband IIR oversampler: flat passband, image rejection, no lookahead", "[fx][dsp]") {
    for (int factor : {2, 4}) {
        dsp::Oversampler os;
        os.prepare(256);
        os.setFactor(factor);
        // Passband: 1 kHz and 15 kHz sine through up->down at unity (+-0.1 dB).
        for (double hz : {1000.0, 15000.0}) {
            os.reset();
            Stereo s = sine(hz, 0.5, 256 * 188);
            std::vector<float> out(s.size());
            for (size_t off = 0; off < s.size(); off += 256) {
                os.up(s.l.data() + off, 256);
                os.down(out.data() + off, 256);
            }
            const double g = rms(out, 24000, 48000) / rms(s.l, 24000, 48000);
            INFO("factor " << factor << " hz " << hz << " gain dB " << db(g));
            REQUIRE(std::fabs(db(g)) < 0.1);
        }
        // Image rejection: upsampled 10 kHz sine has its image at fs_os - 10 kHz.
        os.reset();
        const size_t n = 8192;
        Stereo s = sine(10000.0, 0.5, n);
        std::vector<float> up;
        for (size_t off = 0; off < n; off += 256) {
            os.up(s.l.data() + off, 256);
            up.insert(up.end(), os.buffer(), os.buffer() + 256 * factor);
        }
        const double osr = kSr * factor;
        const size_t N = 16384;
        const auto spec = spectrumDb(up, up.size() - N, N, true);
        const auto bin = [&](double hz) { return static_cast<size_t>(std::lround(hz / osr * static_cast<double>(N))); };
        double sig = -1e9, img = -1e9;
        for (size_t k = bin(10000.0) - 3; k <= bin(10000.0) + 3; ++k) sig = std::max(sig, spec[k]);
        for (size_t k = bin(38000.0) - 3; k <= bin(38000.0) + 3; ++k) img = std::max(img, spec[k]);
        INFO("factor " << factor << " image rejection " << sig - img);
        REQUIRE(sig - img > 70.0);
    }
}

TEST_CASE("Note divisions", "[fx][dsp]") {
    REQUIRE(dsp::noteDivisionChoices().size() == dsp::kNoteDivisions.size());
    REQUIRE(dsp::noteDivisionSeconds(0, 120.0) == 0.0);
    const auto idx = [](const char* label) {
        for (size_t i = 0; i < dsp::kNoteDivisions.size(); ++i)
            if (std::string(dsp::kNoteDivisions[i].label) == label) return static_cast<int>(i);
        return -1;
    };
    REQUIRE(std::fabs(dsp::noteDivisionSeconds(idx("1/4"), 120.0) - 0.5) < 1e-12);
    REQUIRE(std::fabs(dsp::noteDivisionSeconds(idx("1/8."), 120.0) - 0.375) < 1e-12);
    REQUIRE(std::fabs(dsp::noteDivisionSeconds(idx("1/8T"), 120.0) - 1.0 / 6.0) < 1e-12);
    REQUIRE(std::fabs(dsp::noteDivisionSeconds(idx("1/16"), 90.0) - 1.0 / 6.0) < 1e-12);
}

// --------------------------------------------------------------------------------------------- all effects

TEST_CASE("Effects: registered, kind Effect, zero latency, catalog sane", "[fx]") {
    for (const char* t : kFxTypes) {
        const ModuleInfo* info = defaultRegistry().find(t);
        INFO(t);
        REQUIRE(info);
        REQUIRE(info->kind == ModuleKind::Effect);
        auto m = make(t);
        REQUIRE(m->latencySamples() == 0);
        REQUIRE(m->tailSamples() >= 0);
    }
}

TEST_CASE("Effects: bounded and finite under extreme params (incl. feedback 100%)", "[fx][stress]") {
    const Stereo src = [] {
        Stereo s = noise(48000, 0.8f, 7);
        for (size_t i = 0; i < s.size(); i += 4800) s.l[i] = s.r[i] = 1.0f; // clicks
        return s;
    }();
    uint32_t rng = 12345;
    for (const char* t : kFxTypes) {
        auto m = make(t);
        const auto& specs = m->params().specs();
        // Pass 0: all params at min; 1: all at max; 2..9: random corners.
        for (int pass = 0; pass < 10; ++pass) {
            for (int p = 0; p < static_cast<int>(specs.size()); ++p) {
                const ParamSpec& sp = specs[static_cast<size_t>(p)];
                if (sp.isReadOnly()) continue;
                rng = rng * 1664525u + 1013904223u;
                const bool hi = pass == 1 || (pass >= 2 && (rng >> 16) & 1);
                m->params().set(p, hi ? sp.max : sp.min);
            }
            m->reset();
            Stereo s = src;
            run(*m, s);
            INFO(t << " pass " << pass);
            REQUIRE(allFinite(s));
            // EQ at max: five +18 dB bands stacked at 20 kHz + 12 dB level is legitimately loud.
            const double bound = std::string(t) == "eq" ? 1e4 : 64.0;
            REQUIRE(peakAbs(s.l) < bound);
            REQUIRE(peakAbs(s.r) < bound);
        }
    }
    // Delay at feedback 100 % with sustained loud input stays bounded for 20 s.
    auto d = make("delay", {{"feedback", 1.0f}, {"mix", 1.0f}, {"time", 50.0f}, {"high_cut_hz", 20000.0f},
                            {"low_cut_hz", 20.0f}});
    Stereo s = sine(1000.0, 1.0, static_cast<size_t>(20 * kSr));
    run(*d, s);
    REQUIRE(allFinite(s));
    REQUIRE(peakAbs(s.l) < 3.0);
}

TEST_CASE("Effects: other sample rates / block sizes stay finite and sane", "[fx]") {
    for (double sr : {44100.0, 96000.0, 192000.0})
        for (int block : {32, 512}) {
            for (const char* t : kFxTypes) {
                auto m = make(t, {}, sr, block);
                Stereo s = noise(static_cast<size_t>(sr / 2), 0.5f, 21);
                run(*m, s, 120.0, block, sr);
                INFO(t << " @ " << sr << " / " << block);
                REQUIRE(allFinite(s));
                REQUIRE(peakAbs(s.l) < 8.0);
                REQUIRE(rms(s.l, 0, s.size()) > 1e-3);
            }
        }
}

TEST_CASE("Effects: mix = 0 is bit-transparent (drive: all-pass, flat)", "[fx]") {
    const Stereo src = noise(24000, 0.5f, 3);
    for (const char* t : {"chorus", "ensemble", "phaser", "flanger", "delay", "reverb", "compressor"}) {
        auto m = make(t, {{"mix", 0.0f}});
        Stereo s = src;
        run(*m, s);
        INFO(t);
        REQUIRE(s.l == src.l);
        REQUIRE(s.r == src.r);
    }
    // Flat EQ is bit-transparent too.
    {
        auto m = make("eq");
        Stereo s = src;
        run(*m, s);
        REQUIRE(s.l == src.l);
    }
    // Drive at mix 0: phase-matched dry through the oversampling filters -> flat magnitude, no distortion.
    auto m = make("drive", {{"mix", 0.0f}, {"drive_db", 40.0f}});
    Stereo s = sine(1000.0, 0.9, 48000);
    const Stereo in = s;
    run(*m, s);
    REQUIRE(std::fabs(db(rms(s.l, 24000, 48000) / rms(in.l, 24000, 48000))) < 0.05);
}

TEST_CASE("Effects: tails are honest and silence costs nothing (no denormal spikes)", "[fx]") {
    for (const char* t : kFxTypes) {
        auto m = make(t);
        if (m->params().indexOf("mix") >= 0) setP(*m, "mix", 1.0f);
        if (std::string(t) == "reverb") setP(*m, "decay", 3.0f);
        m->reset();
        const size_t burst = 24000;
        const size_t tail = static_cast<size_t>(m->tailSamples());
        const size_t total = burst + tail + 48000;
        Stereo s(total);
        const Stereo nz = noise(burst, 0.5f, 11);
        std::copy(nz.l.begin(), nz.l.end(), s.l.begin());
        std::copy(nz.r.begin(), nz.r.end(), s.r.begin());
        run(*m, s);
        INFO(t << " tail " << tail);
        // After the reported tail, the output is below -80 dBFS.
        REQUIRE(rms(s.l, burst + tail, total) < 1e-4);
        REQUIRE(rms(s.r, burst + tail, total) < 1e-4);
    }
    // Long silent render (20 s after a burst): per-block cost must not blow up while state decays. Run with
    // FTZ/DAZ as the Engine does on the audio thread (ARCHITECTURE sec. 4.5).
    const dsp::ScopedNoDenormals ftz;
    for (const char* t : kFxTypes) {
        auto m = make(t);
        if (m->params().indexOf("mix") >= 0) setP(*m, "mix", 1.0f);
        if (std::string(t) == "reverb") setP(*m, "decay", 20.0f);
        if (std::string(t) == "delay") setP(*m, "feedback", 0.9f);
        m->reset();
        Stereo active = noise(48000, 0.5f, 5);
        const auto t0 = std::chrono::steady_clock::now();
        run(*m, active);
        const double activeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Stereo silent(static_cast<size_t>(20 * kSr));
        const auto t1 = std::chrono::steady_clock::now();
        run(*m, silent);
        const double silentSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() / 20.0;
        INFO(t << " active " << activeSec * 1e3 << " ms/s, silent " << silentSec * 1e3 << " ms/s");
        REQUIRE(allFinite(silent));
        REQUIRE(silentSec < 3.0 * activeSec + 2e-3);
    }
}

// --------------------------------------------------------------------------------------------- delay

TEST_CASE("Delay: time accuracy in ms and tempo-synced divisions", "[fx][delay]") {
    const auto echoAt = [](Module& m, double tempo) {
        Stereo s(static_cast<size_t>(2.5 * kSr));
        s.l[0] = s.r[0] = 1.0f;
        run(m, s, tempo);
        return argmaxAbs(s.l, 10, s.size());
    };
    const std::initializer_list<std::pair<const char*, float>> clean = {
        {"mix", 1.0f}, {"feedback", 0.0f}, {"high_cut_hz", 20000.0f}, {"low_cut_hz", 20.0f}};
    {
        auto m = make("delay", clean);
        setP(*m, "time", 250.0f);
        m->reset();
        REQUIRE(std::abs(static_cast<long>(echoAt(*m, 120.0)) - 12000) <= 2);
    }
    const auto idx = [](const char* label) {
        for (size_t i = 0; i < dsp::kNoteDivisions.size(); ++i)
            if (std::string(dsp::kNoteDivisions[i].label) == label) return static_cast<float>(i);
        return -1.0f;
    };
    struct Case {
        const char* div;
        double tempo, seconds;
    };
    for (const Case& c : {Case{"1/8.", 120.0, 0.375}, Case{"1/8.", 103.0, 0.75 * 60.0 / 103.0},
                          Case{"1/4T", 100.0, 0.4}, Case{"1/16", 140.0, 0.25 * 60.0 / 140.0},
                          Case{"1/2", 60.0, 2.0}}) {
        auto m = make("delay", clean);
        setP(*m, "sync", idx(c.div));
        ProcessContext ctx;
        ctx.transport.tempo = c.tempo;
        // The first block sees the tempo; the delay glides from the reset value -> settle before the impulse.
        Stereo warm(static_cast<size_t>(kSr));
        run(*m, warm, c.tempo);
        m->reset(); // snaps to the synced time at the last seen tempo
        INFO(c.div << " @ " << c.tempo);
        REQUIRE(std::fabs(m->params().get(DelayFx::Sync) - idx(c.div)) < 0.5f);
        const long expect = std::lround(c.seconds * kSr);
        REQUIRE(std::abs(static_cast<long>(echoAt(*m, c.tempo)) - expect) <= 2);
    }
}

TEST_CASE("Delay: ping-pong alternates channels, feedback decays, tail covers repeats", "[fx][delay]") {
    auto m = make("delay", {{"mode", 1.0f}, {"mix", 1.0f}, {"feedback", 0.5f}, {"time", 100.0f},
                            {"high_cut_hz", 20000.0f}, {"low_cut_hz", 20.0f}});
    Stereo s(static_cast<size_t>(kSr));
    s.l[0] = s.r[0] = 1.0f;
    run(*m, s);
    const size_t T = 4800;
    REQUIRE(peakAbs(s.l, T - 5, T + 5) > 0.4);   // 1st echo left
    REQUIRE(peakAbs(s.r, T - 5, T + 5) < 1e-3);
    REQUIRE(peakAbs(s.r, 2 * T - 5, 2 * T + 5) > 0.2); // 2nd right, x feedback
    REQUIRE(peakAbs(s.l, 2 * T - 5, 2 * T + 5) < 1e-3);
    REQUIRE(peakAbs(s.l, 3 * T - 5, 3 * T + 5) > 0.1);
    REQUIRE(m->tailSamples() > static_cast<int>(14 * T)); // 0.5^15 ~ -90 dB
}

TEST_CASE("Delay: ducking lowers the wet while input plays", "[fx][delay]") {
    auto a = make("delay", {{"mix", 0.5f}, {"duck", 0.0f}, {"feedback", 0.6f}});
    auto b = make("delay", {{"mix", 0.5f}, {"duck", 1.0f}, {"feedback", 0.6f}});
    Stereo sa = sine(440.0, 0.5, 96000), sb = sa;
    run(*a, sa);
    run(*b, sb);
    REQUIRE(rms(sb.l, 48000, 96000) < rms(sa.l, 48000, 96000) * 0.9);
}

// --------------------------------------------------------------------------------------------- reverb

TEST_CASE("Reverb: RT60 within 20% of the decay setting", "[fx][reverb]") {
    struct Case {
        int mode;
        float decay, size;
    };
    for (const Case& c : {Case{ReverbFx::Hall, 2.0f, 0.5f}, Case{ReverbFx::Hall, 6.0f, 1.0f},
                          Case{ReverbFx::Plate, 3.0f, 0.5f}, Case{ReverbFx::Room, 0.8f, 0.3f},
                          Case{ReverbFx::Chamber, 1.5f, 0.6f}}) {
        auto m = make("reverb", {{"mode", static_cast<float>(c.mode)}, {"decay", c.decay}, {"size", c.size},
                                 {"mix", 1.0f}, {"early", 0.0f}, {"predelay_ms", 0.0f}, {"damp_hf", 0.0f},
                                 {"damp_lf", 0.0f}, {"high_cut_hz", 20000.0f}});
        Stereo s(static_cast<size_t>((c.decay * 1.6 + 0.5) * kSr));
        s.l[0] = s.r[0] = 1.0f;
        run(*m, s);
        const double t = rt60(s.l, kSr);
        INFO("mode " << c.mode << " decay " << c.decay << " measured " << t);
        REQUIRE(t > 0.8 * c.decay);
        REQUIRE(t < 1.2 * c.decay);
    }
}

TEST_CASE("Reverb: HF damping shortens the HF decay; L/R decorrelated", "[fx][reverb]") {
    auto m = make("reverb", {{"decay", 3.0f}, {"mix", 1.0f}, {"damp_hf", 0.8f}, {"high_cut_hz", 20000.0f}});
    Stereo s(static_cast<size_t>(4 * kSr));
    s.l[0] = s.r[0] = 1.0f;
    run(*m, s);
    // Energy above ~6 kHz in a late window vs. an early window drops much more than broadband.
    const auto bandRatio = [&](size_t a) {
        const auto sp = spectrumDb(s.l, a, 8192, true);
        double lo = 0.0, hi = 0.0;
        for (size_t k = 0; k < sp.size(); ++k) {
            const double hz = static_cast<double>(k) * kSr / 8192.0, p = std::pow(10.0, sp[k] / 10.0);
            if (hz > 200.0 && hz < 1500.0) lo += p;
            if (hz > 6000.0 && hz < 16000.0) hi += p;
        }
        return 10.0 * std::log10(hi / lo);
    };
    REQUIRE(bandRatio(static_cast<size_t>(1.2 * kSr)) < bandRatio(static_cast<size_t>(0.1 * kSr)) - 10.0);
    // Decorrelation of the late field.
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (size_t i = 4800; i < 48000; ++i) {
        lr += static_cast<double>(s.l[i]) * s.r[i];
        ll += static_cast<double>(s.l[i]) * s.l[i];
        rr += static_cast<double>(s.r[i]) * s.r[i];
    }
    REQUIRE(std::fabs(lr / std::sqrt(ll * rr)) < 0.3);
}

TEST_CASE("Reverb: dense, non-metallic late field (echo density + spectral statistics)", "[fx][reverb]") {
    // Room: shorter lines (~0.3 modes/Hz) -> tested at a room-like RT60 (density criterion ~0.15 x RT60).
    for (int mode : {ReverbFx::Hall, ReverbFx::Plate, ReverbFx::Room}) {
        const float decay = mode == ReverbFx::Room ? 1.0f : 2.5f;
        auto m = make("reverb", {{"mode", static_cast<float>(mode)}, {"decay", decay}, {"mix", 1.0f},
                                 {"early", 0.0f}, {"predelay_ms", 0.0f}, {"high_cut_hz", 20000.0f}});
        Stereo s(static_cast<size_t>(2 * kSr));
        s.l[0] = s.r[0] = 1.0f;
        run(*m, s);
        // Normalized echo density (Abel & Huang) in 20 ms windows from 150 ms on: ~1 for Gaussian-dense.
        double nedMin = 2.0;
        for (size_t a = 7200; a + 960 < 24000; a += 960) {
            const double sd = rms(s.l, a, a + 960);
            int outside = 0;
            for (size_t i = a; i < a + 960; ++i) outside += std::fabs(s.l[i]) > sd ? 1 : 0;
            nedMin = std::min(nedMin, (outside / 960.0) / 0.3173);
        }
        // Spectral statistics of a late 0.68 s segment, 1..6 kHz: a dense modal field has a Rayleigh magnitude
        // (std of dB ~5.6); sparse/metallic ringing shows up as a much larger spread and high peaks.
        const size_t N = 32768;
        const auto sp = spectrumDb(s.l, static_cast<size_t>(0.2 * kSr), N, true);
        std::vector<double> band;
        for (size_t k = 0; k < sp.size(); ++k) {
            const double hz = static_cast<double>(k) * kSr / static_cast<double>(N);
            if (hz > 1000.0 && hz < 6000.0) band.push_back(sp[k]);
        }
        // Remove the slow spectral envelope (moving average over ~300 Hz).
        std::vector<double> dev;
        const size_t w = 100;
        for (size_t i = w; i + w < band.size(); ++i) {
            double mean = 0.0;
            for (size_t j = i - w; j <= i + w; ++j) mean += band[j];
            dev.push_back(band[i] - mean / static_cast<double>(2 * w + 1));
        }
        double sq = 0.0, mx = -1e9;
        for (double d : dev) {
            sq += d * d;
            mx = std::max(mx, d);
        }
        const double sdDb = std::sqrt(sq / static_cast<double>(dev.size()));
        // Modal density: local spectral maxima per Hz vs. the same analysis of exponentially decaying white noise
        // (the resolution limit, ~0.2/Hz here). The FDN has ~1 mode/Hz (Hall), so it must look as dense as noise.
        const auto peaksPerHz = [&](const std::vector<float>& x) {
            const auto spx = spectrumDb(x, static_cast<size_t>(0.2 * kSr), N, true);
            int peaks = 0;
            for (size_t k = 1; k + 1 < spx.size(); ++k) {
                const double hz = static_cast<double>(k) * kSr / static_cast<double>(N);
                if (hz > 1000.0 && hz < 6000.0 && spx[k] > spx[k - 1] && spx[k] > spx[k + 1]) ++peaks;
            }
            return peaks / 5000.0;
        };
        Stereo ref = noise(s.size(), 0.5f, 77);
        for (size_t i = 0; i < ref.size(); ++i)
            ref.l[i] *= static_cast<float>(std::pow(10.0, -3.0 * static_cast<double>(i) / (decay * kSr)));
        const double density = peaksPerHz(s.l), refDensity = peaksPerHz(ref.l);
        INFO("mode " << mode << " NED min " << nedMin << " spectral sd " << sdDb << " dB, max peak " << mx
                     << " dB, peaks/Hz " << density << " (noise " << refDensity << ")");
        REQUIRE(nedMin > 0.75);
        REQUIRE(sdDb < 7.0);
        REQUIRE(mx < 15.0);
        REQUIRE(density > 0.85 * refDensity);
    }
}

TEST_CASE("Reverb: gated mode cuts the tail after the hold", "[fx][reverb]") {
    auto m = make("reverb", {{"mode", static_cast<float>(ReverbFx::Gated)}, {"decay", 3.0f}, {"mix", 1.0f},
                             {"predelay_ms", 0.0f}, {"gate_threshold_db", -30.0f}, {"gate_hold_ms", 200.0f}});
    Stereo s(static_cast<size_t>(1.5 * kSr));
    for (size_t i = 0; i < 2400; ++i) s.l[i] = s.r[i] = 0.5f * std::sin(0.13f * static_cast<float>(i)); // 50 ms hit
    run(*m, s);
    const double during = rms(s.l, 4800, 9600);              // 100..200 ms: open
    const double after = rms(s.l, 20000, 30000);             // > 50 + 30 (env) + 200 + release
    INFO("during " << db(during) << " after " << db(after));
    REQUIRE(during > 0.01);
    REQUIRE(after < during * 1e-3);
    REQUIRE(m->tailSamples() < static_cast<int>(kSr)); // honest, short
}

TEST_CASE("Reverb: shimmer stays bounded and adds energy above the input band", "[fx][reverb]") {
    auto m = make("reverb", {{"mode", static_cast<float>(ReverbFx::ShimmerMode)}, {"decay", 20.0f},
                             {"shimmer", 1.0f}, {"mix", 1.0f}, {"damp_hf", 0.0f}});
    Stereo s = sine(440.0, 0.8, static_cast<size_t>(10 * kSr));
    run(*m, s);
    REQUIRE(allFinite(s));
    REQUIRE(peakAbs(s.l) < 8.0);
    const auto sp = spectrumDb(s.l, static_cast<size_t>(8 * kSr), 16384, true);
    const auto at = [&](double hz) {
        double mx = -1e9;
        const size_t k = static_cast<size_t>(hz / kSr * 16384.0);
        for (size_t j = k - 4; j <= k + 4; ++j) mx = std::max(mx, sp[j]);
        return mx;
    };
    REQUIRE(at(880.0) > at(440.0) - 30.0); // octave up present
}

// --------------------------------------------------------------------------------------------- eq

TEST_CASE("EQ: bell / shelf / cut gains at their corners", "[fx][eq]") {
    const auto gainAt = [](Module& m, double hz) {
        m.reset();
        Stereo s = sine(hz, 0.25, 48000);
        const Stereo in = s;
        run(m, s);
        return db(rms(s.l, 24000, 48000) / rms(in.l, 24000, 48000));
    };
    auto m = make("eq", {{"p2_hz", 1000.0f}, {"p2_db", 6.0f}, {"p2_q", 1.0f}});
    REQUIRE(std::fabs(gainAt(*m, 1000.0) - 6.0) < 0.5);
    REQUIRE(std::fabs(gainAt(*m, 100.0)) < 0.5);
    setP(*m, "p2_db", -12.0f);
    setP(*m, "p2_hz", 3000.0f);
    REQUIRE(std::fabs(gainAt(*m, 3000.0) + 12.0) < 0.5);
    for (const char* b : {"p1", "p3"}) {
        auto e = make("eq", {{(std::string(b) + "_hz").c_str(), 500.0f}, {(std::string(b) + "_db").c_str(), 9.0f},
                             {(std::string(b) + "_q").c_str(), 4.0f}});
        REQUIRE(std::fabs(gainAt(*e, 500.0) - 9.0) < 0.5);
    }
    auto ls = make("eq", {{"ls_hz", 200.0f}, {"ls_db", 6.0f}});
    REQUIRE(std::fabs(gainAt(*ls, 20.0) - 6.0) < 0.5);
    REQUIRE(std::fabs(gainAt(*ls, 200.0) - 3.0) < 0.5); // half gain at the corner
    REQUIRE(std::fabs(gainAt(*ls, 8000.0)) < 0.5);
    auto hs = make("eq", {{"hs_hz", 4000.0f}, {"hs_db", -8.0f}});
    REQUIRE(std::fabs(gainAt(*hs, 18000.0) + 8.0) < 0.5);
    REQUIRE(std::fabs(gainAt(*hs, 4000.0) + 4.0) < 0.5);
    auto hp = make("eq", {{"hp_on", 1.0f}, {"hp_hz", 100.0f}});
    REQUIRE(std::fabs(gainAt(*hp, 100.0) + 3.01) < 0.5);
    REQUIRE(gainAt(*hp, 25.0) < -20.0);
    setP(*hp, "cut_slope", 1.0f);
    REQUIRE(std::fabs(gainAt(*hp, 100.0) + 3.01) < 0.5);
    REQUIRE(gainAt(*hp, 25.0) < -44.0);
    auto lp = make("eq", {{"lp_on", 1.0f}, {"lp_hz", 5000.0f}, {"level_db", -6.0f}});
    REQUIRE(std::fabs(gainAt(*lp, 5000.0) + 9.01) < 0.5);
}

TEST_CASE("EQ: stable under fast parameter modulation", "[fx][eq]") {
    auto m = make("eq", {{"p2_db", 18.0f}, {"p2_q", 10.0f}, {"hp_on", 1.0f}, {"lp_on", 1.0f}, {"cut_slope", 1.0f}});
    Stereo s = noise(96000, 0.5f, 9);
    static const ChannelState cs{};
    ProcessContext ctx;
    ctx.channel = &cs;
    for (size_t off = 0; off < s.size(); off += 32) {
        const float t = static_cast<float>(off) / 96000.0f;
        m->params().set("p2_hz", 20.0f * std::pow(1000.0f, 0.5f + 0.5f * std::sin(60.0f * t)));
        m->params().set("hp_hz", 10.0f * std::pow(100.0f, 0.5f + 0.5f * std::sin(37.0f * t)));
        m->params().set("lp_hz", 1000.0f * std::pow(20.0f, 0.5f + 0.5f * std::sin(23.0f * t)));
        AudioBlock b{s.l.data() + off, s.r.data() + off, 32};
        ctx.numSamples = 32;
        m->process(b, {}, ctx);
    }
    REQUIRE(allFinite(s));
    REQUIRE(peakAbs(s.l) < 20.0);
}

// --------------------------------------------------------------------------------------------- compressor

TEST_CASE("Compressor: VCA static curve, knee, makeup, gr_db meter", "[fx][compressor]") {
    REQUIRE(CompressorFx::moduleInfo().params[CompressorFx::GainReductionDb].isReadOnly());
    for (float knee : {0.0f, 12.0f}) {
        for (double inDb : {-40.0, -26.0, -20.0, -14.0, -6.0, 0.0}) {
            auto m = make("compressor", {{"threshold_db", -20.0f}, {"ratio", 4.0f}, {"attack_ms", 1.0f},
                                         {"release_ms", 2000.0f}, {"knee_db", knee}, {"makeup_db", 3.0f}});
            Stereo s = sine(1000.0, std::pow(10.0, inDb / 20.0), static_cast<size_t>(1.5 * kSr));
            run(*m, s);
            const double outDb = db(peakAbs(s.l, 60000, 72000));
            const double expect =
                inDb + CompressorFx::staticGainDb(static_cast<float>(inDb), -20.0f, 4.0f, knee) + 3.0;
            INFO("knee " << knee << " in " << inDb << " out " << outDb << " expect " << expect);
            REQUIRE(std::fabs(outDb - expect) < 0.5);
            const double gr = m->params().get(CompressorFx::GainReductionDb);
            REQUIRE(std::fabs(gr + CompressorFx::staticGainDb(static_cast<float>(inDb), -20.0f, 4.0f, knee)) < 0.6);
        }
    }
}

TEST_CASE("Compressor: attack/release timing and opto program-dependent release", "[fx][compressor]") {
    // Step from -40 to -10 dBFS: GR reaches ~63% of its final value about one attack time later.
    auto m = make("compressor", {{"threshold_db", -30.0f}, {"ratio", 10.0f}, {"attack_ms", 20.0f},
                                 {"release_ms", 200.0f}, {"knee_db", 0.0f}});
    Stereo s(static_cast<size_t>(kSr));
    for (size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = (i < 24000 ? 0.01f : 0.316f) * ((i & 1) ? 1.0f : -1.0f);
    run(*m, s);
    const double final = db(std::fabs(s.l[47999]) / 0.316);
    const double at20 = db(std::fabs(s.l[24000 + 960]) / 0.316);
    REQUIRE(final < -15.0);
    REQUIRE(std::fabs(at20 / final - 0.63) < 0.12);

    // Opto: after a long loud passage the release is slower than after a short one.
    const auto recovery = [](size_t loudSamples) {
        auto o = make("compressor", {{"mode", 1.0f}, {"threshold_db", -30.0f}, {"ratio", 4.0f}});
        Stereo x(loudSamples + static_cast<size_t>(kSr));
        for (size_t i = 0; i < x.size(); ++i) x.l[i] = x.r[i] = (i < loudSamples ? 0.5f : 0.02f) * ((i & 1) ? 1.f : -1.f);
        run(*o, x);
        return db(std::fabs(x.l[loudSamples + 9600]) / 0.02); // gain 200 ms after the loud part
    };
    REQUIRE(recovery(static_cast<size_t>(4 * kSr)) < recovery(static_cast<size_t>(0.05 * kSr)) - 1.0);
}

// --------------------------------------------------------------------------------------------- drive

TEST_CASE("Drive: oversampling lowers aliasing; harmonics; no lookahead", "[fx][drive]") {
    const size_t N = 16384;
    // Bin-aligned fundamental near 3 kHz, not a divisor of fs (aliases must not land on harmonic bins).
    const double f0 = 1031.0;
    const double hz = f0 * kSr / static_cast<double>(N);
    const auto aliasRatio = [&](int mode, float os) {
        auto m = make("drive", {{"mode", static_cast<float>(mode)}, {"drive_db", 20.0f}, {"oversample", os}});
        Stereo s = sine(hz, 0.5, 2 * N);
        run(*m, s);
        const auto sp = spectrumDb(s.l, N, N, true);
        const size_t k0 = static_cast<size_t>(std::round(f0));
        double harm = 0.0, alias = 0.0;
        // Audible band only: harmonics in the half-band transition right below fs/2 (e.g. the 8th at 24.2 kHz)
        // fold to just under Nyquist; that is filter transition, not in-band aliasing.
        for (size_t k = 5; k < sp.size(); ++k) {
            if (static_cast<double>(k) * kSr / static_cast<double>(N) > 20000.0) break;
            const double p = std::pow(10.0, sp[k] / 10.0);
            bool isHarm = false;
            for (size_t h = k0; h < sp.size(); h += k0)
                if (k + 3 >= h && k <= h + 3) isHarm = true;
            (isHarm ? harm : alias) += p;
        }
        return 10.0 * std::log10(alias / harm);
    };
    for (int mode : {DriveFx::Tube, DriveFx::Fuzz, DriveFx::Tape}) {
        const double a1 = aliasRatio(mode, 0.0f), a2 = aliasRatio(mode, 1.0f), a4 = aliasRatio(mode, 2.0f);
        INFO("mode " << mode << " alias/harmonic: 1x " << a1 << " dB, 2x " << a2 << " dB, 4x " << a4 << " dB");
        REQUIRE(a2 < a1 - 10.0);
        REQUIRE(a4 < a2 - 10.0);
        // Fuzz is close to a square wave (slowly decaying harmonics far past the 4x Nyquist): looser bound.
        REQUIRE(a4 < (mode == DriveFx::Fuzz ? -45.0 : -80.0));
    }
    // Impulse: the IIR group delay is a few samples, no block/lookahead latency.
    auto m = make("drive", {{"drive_db", 0.0f}, {"mode", 2.0f}});
    REQUIRE(m->latencySamples() == 0);
    Stereo s(512);
    s.l[100] = s.r[100] = 0.1f;
    run(*m, s);
    REQUIRE(argmaxAbs(s.l, 0, 512) - 100 < 8);
    // Asymmetry adds even harmonics.
    auto sym = make("drive", {{"mode", 2.0f}, {"asymmetry", 0.0f}, {"drive_db", 20.0f}});
    auto asym = make("drive", {{"mode", 2.0f}, {"asymmetry", 1.0f}, {"drive_db", 20.0f}});
    Stereo a = sine(hz, 0.5, 2 * N), b = a;
    run(*sym, a);
    run(*asym, b);
    const auto h2 = [&](const Stereo& x) {
        return spectrumDb(x.l, N, N, true)[2 * static_cast<size_t>(std::round(f0))];
    };
    REQUIRE(h2(b) > h2(a) + 20.0);
}

// --------------------------------------------------------------------------------------------- modulation

namespace {
// Click train through a wet-only modulated delay: returns per-click delay (ms) for L and R.
void clickDelays(Module& m, double seconds, std::vector<double>& dl, std::vector<double>& dr) {
    const size_t period = 960; // 20 ms
    Stereo s(static_cast<size_t>(seconds * kSr));
    for (size_t i = 0; i < s.size(); i += period) s.l[i] = s.r[i] = 1.0f;
    run(m, s);
    for (size_t i = period * 4; i + period < s.size(); i += period) {
        dl.push_back(static_cast<double>(argmaxAbs(s.l, i, i + period) - i) / kSr * 1000.0);
        dr.push_back(static_cast<double>(argmaxAbs(s.r, i, i + period) - i) / kSr * 1000.0);
    }
}
} // namespace

TEST_CASE("Chorus: Juno modulation depth and opposite-phase stereo", "[fx][chorus]") {
    struct Case {
        int mode;
        double lo, hi;
    };
    for (const Case& c : {Case{ChorusFx::JunoI, 1.66, 5.35}, Case{ChorusFx::JunoII, 1.66, 5.35},
                          Case{ChorusFx::JunoI_II, 3.30, 3.70}}) {
        auto m = make("chorus", {{"mode", static_cast<float>(c.mode)}, {"mix", 1.0f}, {"hiss", 0.0f}});
        std::vector<double> dl, dr;
        clickDelays(*m, 4.5, dl, dr);
        const auto [mn, mx] = std::minmax_element(dl.begin(), dl.end());
        // BBD filters add ~0.1 ms of group delay; the click train samples the LFO sparsely for I+II.
        INFO("mode " << c.mode << " range " << *mn << " .. " << *mx);
        REQUIRE(*mn > c.lo - 0.1);
        REQUIRE(*mn < c.lo + 0.35);
        REQUIRE(*mx > c.hi - 0.35);
        REQUIRE(*mx < c.hi + 0.25);
        if (c.mode != ChorusFx::JunoI_II) {
            // Opposite phase: L + R delay stays ~constant (= lo + hi).
            for (size_t i = 0; i < dl.size(); ++i) REQUIRE(std::fabs(dl[i] + dr[i] - (c.lo + c.hi) - 0.2) < 0.4);
        }
    }
}

TEST_CASE("Ensemble: three-phase modulation, wide stereo", "[fx][ensemble]") {
    auto m = make("ensemble", {{"mix", 1.0f}, {"depth", 1.0f}, {"depth_fast", 1.0f}, {"hiss", 0.0f}});
    std::vector<double> dl, dr;
    clickDelays(*m, 4.0, dl, dr);
    const auto [mn, mx] = std::minmax_element(dl.begin(), dl.end());
    REQUIRE(*mn > 2.5);
    REQUIRE(*mx < 12.0);
    REQUIRE(*mx - *mn > 3.0);
    // Sustained tone: L and R differ (not a mono vibrato).
    auto e = make("ensemble", {{"mix", 1.0f}});
    Stereo s = sine(440.0, 0.5, 96000);
    run(*e, s);
    double diff = 0.0;
    for (size_t i = 48000; i < 96000; ++i) diff += std::fabs(s.l[i] - s.r[i]);
    REQUIRE(diff / 48000.0 > 0.02);
}

TEST_CASE("Phaser: notch count = stages / 2", "[fx][phaser]") {
    for (int choice = 0; choice < 4; ++choice) {
        const int stages = PhaserFx::stageCount(choice);
        auto m = make("phaser", {{"stages", static_cast<float>(choice)}, {"depth", 0.0f}, {"feedback", 0.0f},
                                 {"mix", 0.5f}, {"centre_hz", 1000.0f}});
        const size_t N = 65536;
        Stereo s(N);
        s.l[0] = s.r[0] = 1.0f;
        run(*m, s);
        const auto sp = spectrumDb(s.l, 0, N, false);
        int notches = 0;
        bool in = false;
        for (size_t k = 1; k < sp.size(); ++k) {
            const double hz = static_cast<double>(k) * kSr / static_cast<double>(N);
            if (hz < 10.0) continue;
            const bool deep = sp[k] < -30.0;
            if (deep && !in) ++notches;
            in = deep;
        }
        INFO("stages " << stages);
        REQUIRE(notches == stages / 2);
    }
}

TEST_CASE("Phaser / flanger: tempo sync sets the LFO period", "[fx][phaser][flanger]") {
    // Synced to 1/4 at 120 bpm: the sweep has a 0.5 s period.
    float syncIdx = -1.0f;
    for (size_t i = 0; i < dsp::kNoteDivisions.size(); ++i)
        if (std::string(dsp::kNoteDivisions[i].label) == "1/4") syncIdx = static_cast<float>(i);
    for (const char* t : {"phaser", "flanger"}) {
        auto m = make(t, {{"sync", syncIdx}, {"mix", 0.5f}, {"depth", 1.0f}, {"feedback", 0.0f}});
        // A static tone through the sweeping notches: its level envelope repeats with the LFO period.
        Stereo s = sine(2000.0, 0.3, static_cast<size_t>(3 * kSr));
        run(*m, s, 120.0);
        std::vector<double> env;
        for (size_t a = 0; a + 480 < s.size(); a += 480) env.push_back(rms(s.l, a, a + 480));
        const auto corr = [&](size_t lag) {
            double num = 0.0, d1 = 0.0, d2 = 0.0;
            const double mean = std::accumulate(env.begin(), env.end(), 0.0) / static_cast<double>(env.size());
            for (size_t i = 0; i + lag < env.size(); ++i) {
                num += (env[i] - mean) * (env[i + lag] - mean);
                d1 += (env[i] - mean) * (env[i] - mean);
                d2 += (env[i + lag] - mean) * (env[i + lag] - mean);
            }
            return num / std::sqrt(d1 * d2);
        };
        INFO(t << " corr@0.5s " << corr(50) << " corr@0.25s " << corr(25));
        REQUIRE(corr(50) > 0.6);
        REQUIRE(corr(50) > corr(25) + 0.3);
    }
}

TEST_CASE("Flanger: through-zero reference and feedback polarity", "[fx][flanger]") {
    // Through-zero at mix 0 = the reference line: the signal delayed by `time`.
    auto m = make("flanger", {{"through_zero", 1.0f}, {"mix", 0.0f}, {"time", 3.0f}});
    Stereo s(4800);
    s.l[100] = s.r[100] = 1.0f;
    run(*m, s);
    const double d = static_cast<double>(argmaxAbs(s.l, 0, 4800) - 100) / kSr * 1000.0;
    REQUIRE(std::fabs(d - 3.0) < 0.15);
    // Negative feedback inverts the second echo relative to positive feedback.
    const auto secondEcho = [](float fb) {
        auto f = make("flanger", {{"feedback", fb}, {"mix", 1.0f}, {"depth", 0.0f}, {"time", 5.0f}});
        Stereo x(4800);
        x.l[0] = x.r[0] = 1.0f;
        run(*f, x);
        return x.l[argmaxAbs(x.l, 400, 600)];
    };
    REQUIRE(secondEcho(0.7f) > 0.1f);
    REQUIRE(secondEcho(-0.7f) < -0.1f);
}

TEST_CASE("Effects in an Engine chain: no RT violations", "[fx][rt]") {
    // Process every effect on a pseudo-audio thread with the RT allocation checker armed.
    std::vector<std::unique_ptr<Module>> chain;
    for (const char* t : kFxTypes) chain.push_back(make(t));
    Stereo s = noise(static_cast<size_t>(kSr), 0.3f, 2);
    const uint64_t before = rt::violationCount();
    {
        rt::RtScope scope;
        for (auto& m : chain) run(*m, s);
    }
    REQUIRE(rt::violationCount() == before);
    REQUIRE(allFinite(s));
}
