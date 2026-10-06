#pragma once
// One synthesized drum hit (used by DrumKit). A voice is configured at trigger time from a Recipe (pure data,
// built by makeRecipe() for {instrument, model, tune, decay, tone, velocity}) and rendered with a fixed topology:
//   body:  sine (or triangle-ish) oscillator with exponential pitch sweep + optional 2nd partial, 2-stage amp env
//   metal: up to 6 PolyBLEP squares (808-style cymbal/hat/cowbell bank)
//   noise: white noise
//   metal+noise -> SVF (LP/BP/HP) -> optional extra HP -> 2-stage env or clap-style multi-burst env
//   click: short decaying low-passed noise burst
//   sum -> optional tanh drive -> optional post low-pass -> optional gate (hold + 12 ms fade) -> end window
// The end window (1 - t/T)^2 with T = 6 x the longest time constant guarantees every voice reaches true silence.
// RT-safe: no allocation; everything is a member.

#include "dsp/Math.h"
#include "dsp/PolyBlep.h"
#include "dsp/Svf.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace ks::drums {

enum class Instr : int { Kick, Snare, Clap, ClosedHat, OpenHat, Crash, Ride, TomLo, TomMid, TomHi, Rim, Cowbell, Tamb, Count };
enum class Model : int { Tr808, Tr909, Linn, Industrial, Count };
inline constexpr int kNumInstr = static_cast<int>(Instr::Count);

struct Recipe {
    // body
    float bodyHz = 0.0f, bodyLevel = 0.0f, body2Ratio = 0.0f, body2Level = 0.0f;
    float sweep = 0.0f, sweepTau = 0.01f;    // f = bodyHz * (1 + sweep * exp(-t / sweepTau))
    float bodyTau = 0.2f, bodyTau2 = 0.0f, bodyMix2 = 0.0f;
    bool bodyTri = false;                    // soft-clipped (triangle-ish) body instead of a pure sine
    // metal bank
    int metalCount = 0;
    std::array<float, 6> metalHz{};
    float metalLevel = 0.0f;
    // noise
    float noiseLevel = 0.0f;
    // metal+noise filter
    dsp::Svf::Mode nMode = dsp::Svf::Mode::BandPass;
    float nCut = 5000.0f, nRes = 0.1f, hpCut = 0.0f;
    float noiseTau = 0.1f, noiseTau2 = 0.0f, noiseMix2 = 0.0f;
    int bursts = 0;                          // clap: number of bursts before the tail
    float burstSpacing = 0.01f, burstTau = 0.003f;
    // click
    float clickLevel = 0.0f, clickTau = 0.002f, clickCut = 4000.0f;
    // shaping
    float drive = 0.0f;   // 0 = off, else tanh(x * drive) / tanh(drive)
    float postLp = 0.0f;  // 0 = off
    float gate = 0.0f;    // s, 0 = off
    float gain = 1.0f;
};

// Builds the recipe. tuneSt semitones, decayScale > 0, tone 0..1, vel 0..1 (after velocity curve: only used for
// brightness; amplitude is applied by the caller).
Recipe makeRecipe(Instr instr, Model model, float tuneSt, float decayScale, float tone, float vel, bool rideBell);

class DrumVoice {
public:
    void setSampleRate(double sr) noexcept;
    void trigger(const Recipe& r, float amp, float gainL, float gainR, uint32_t seed) noexcept;
    // Fast fade-out (choke / retrigger), ~4 ms.
    void choke() noexcept;
    void kill() noexcept { active_ = false; }
    bool active() const noexcept { return active_; }
    bool choking() const noexcept { return chokeStep_ > 0.0f; }
    int64_t age() const noexcept { return age_; }
    // Adds into left/right.
    void render(float* left, float* right, int n) noexcept;

private:
    float noise() noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
    }

    float sr_ = 48000.0f, invSr_ = 1.0f / 48000.0f;
    bool active_ = false;
    Recipe r_;
    int64_t age_ = 0, length_ = 1;
    float invLength_ = 1.0f;
    float amp_ = 0.0f, gl_ = 0.0f, gr_ = 0.0f;
    // envelopes (multiplicative per-sample decays)
    float pEnv_ = 0.0f, pDec_ = 0.0f;
    float bEnv1_ = 0.0f, bDec1_ = 0.0f, bEnv2_ = 0.0f, bDec2_ = 0.0f;
    float nEnv1_ = 0.0f, nDec1_ = 0.0f, nEnv2_ = 0.0f, nDec2_ = 0.0f;
    float cEnv_ = 0.0f, cDec_ = 0.0f;
    float burstDec_ = 0.0f, burstEnv_ = 0.0f;
    int64_t burstLen_ = 1, burstEnd_ = 0;
    int64_t gateAt_ = -1;
    float gateEnv_ = 1.0f, gateDec_ = 1.0f;
    float driveNorm_ = 1.0f;
    // oscillators / filters
    double phase1_ = 0.0, phase2_ = 0.0;
    std::array<dsp::PolyBlepOsc, 6> metal_{};
    dsp::Svf nf_, hp_, clickLp_, post_;
    uint32_t rng_ = 0x12345678u;
    float choke_ = 1.0f, chokeStep_ = 0.0f;
};

} // namespace ks::drums
