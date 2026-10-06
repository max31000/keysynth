#include "effects/rotary/RotaryFx.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
constexpr float kHornRadiusSec = 0.00044f;  // ~0.15 m / 343 m/s
constexpr float kDrumRadiusSec = 0.00015f;  // baffle: much less Doppler, mostly AM
constexpr float kReflSec = 0.0009f;         // cabinet reflection path, extra delay
constexpr float kHornAm = 0.55f, kDrumAm = 0.35f, kReflGain = 0.3f;
constexpr float kCrossoverHz = 800.0f;

float onePole(float hz, double sr) noexcept { return 1.0f - std::exp(-dsp::kTwoPi * hz / static_cast<float>(sr)); }
} // namespace

const ModuleInfo& RotaryFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "rotary";
        i.displayName = "Rotary Speaker";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        auto ro = [](ParamSpec s) {
            s.flags = ParamFlags::ReadOnly | ParamFlags::NonAutomatable;
            return s;
        };
        i.params = {
            enumParam("speed", "Speed", {"Slow", "Fast", "Stop"}, 0, "Speed"),
            boolParam("mod_wheel", "Mod Wheel = Fast", true, "Speed"),
            boolParam("sustain_toggle", "Sustain Toggles", false, "Speed"),
            linearParam("drive", "Drive", 0.0f, 1.0f, 0.15f, {}, "Amp"),
            linearParam("balance", "Horn/Drum", -1.0f, 1.0f, 0.0f, {}, "Amp"),
            linearParam("spread", "Mic Spread", 0.0f, 1.0f, 0.7f, {}, "Mics"),
            linearParam("horn_slow_rpm", "Horn Slow", 20.0f, 80.0f, 48.0f, "rpm", "Rotors"),
            linearParam("horn_fast_rpm", "Horn Fast", 300.0f, 450.0f, 400.0f, "rpm", "Rotors"),
            linearParam("drum_slow_rpm", "Drum Slow", 20.0f, 80.0f, 40.0f, "rpm", "Rotors"),
            linearParam("drum_fast_rpm", "Drum Fast", 250.0f, 420.0f, 342.0f, "rpm", "Rotors"),
            logParam("inertia", "Inertia", 0.25f, 4.0f, 1.0f, "x", "Rotors", 1.0f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 1.0f, {}, "Output"),
            linearParam("level_db", "Level", -24.0f, 12.0f, 0.0f, "dB", "Output"),
            ro(linearParam("horn_rpm", "Horn RPM", 0.0f, 500.0f, 0.0f, "rpm", "Readout")),
            ro(linearParam("drum_rpm", "Drum RPM", 0.0f, 500.0f, 0.0f, "rpm", "Readout")),
        };
        i.uiHints = {{"groupOrder", {"Speed", "Amp", "Mics", "Rotors", "Output", "Readout"}},
                     {"front", {"speed", "drive", "balance", "spread", "mix", "horn_rpm", "drum_rpm"}}};
        return i;
    }();
    return info;
}

RotaryFx::RotaryFx() : Module(moduleInfo()) {}

void RotaryFx::prepare(double sampleRate, int /*maxBlock*/) {
    sampleRate_ = sampleRate;
    const float sr = static_cast<float>(sampleRate);
    preamp_.prepare(sampleRate);
    xover_.prepare(sampleRate, kCrossoverHz);
    hornCab_.setSampleRate(sampleRate);
    hornCab_.setCutoff(6500.0f, 0.12f);
    drumCab_.setSampleRate(sampleRate);
    drumCab_.setCutoff(110.0f, 0.45f);
    hornDepth_ = kHornRadiusSec * sr;
    drumDepth_ = kDrumRadiusSec * sr;
    reflDelay_ = kReflSec * sr;
    hornLine_.prepare(static_cast<int>(2.0f + reflDelay_ + 2.0f * hornDepth_) + 8);
    drumLine_.prepare(static_cast<int>(2.0f + 2.0f * drumDepth_) + 8);
    lpFacing_ = onePole(14000.0f, sampleRate);
    lpAway_ = onePole(3200.0f, sampleRate);
    mix_.prepare(sampleRate, 0.02f);
    level_.prepare(sampleRate, 0.02f);
    spread_.prepare(sampleRate, 0.05f);
    balance_.prepare(sampleRate, 0.02f);
    reset();
}

void RotaryFx::reset() {
    const ParamSet& p = params();
    const bool stop = static_cast<int>(p.get(Speed)) == Stop;
    const bool fast = static_cast<int>(p.get(Speed)) == Fast;
    horn_.rpm = stop ? 0.0f : p.get(fast ? HornFastRpm : HornSlowRpm);
    drum_.rpm = stop ? 0.0f : p.get(fast ? DrumFastRpm : DrumSlowRpm);
    horn_.angle = 0.0f;
    drum_.angle = 1.3f;
    preamp_.reset();
    xover_.reset();
    hornCab_.reset();
    drumCab_.reset();
    hornLine_.reset();
    drumLine_.reset();
    dirLp_[0] = dirLp_[1] = reflLp_[0] = reflLp_[1] = 0.0f;
    mix_.snap(p.get(Mix));
    level_.snap(dsp::dbToGain(p.get(LevelDb)));
    spread_.snap(p.get(Spread));
    balance_.snap(p.get(Balance));
    lastSustain_ = false;
    toggled_ = false;
    params().setRaw(HornRpm, horn_.rpm);
    params().setRaw(DrumRpm, drum_.rpm);
}

void RotaryFx::process(AudioBlock& io, MidiEventSpan /*events*/, const ProcessContext& ctx) {
    const ParamSet& p = params();
    const int mode = static_cast<int>(p.get(Speed));
    const ChannelState* ch = ctx.channel;
    const bool sus = ch && ch->sustain;
    if (p.get(SustainToggle) >= 0.5f) {
        if (sus && !lastSustain_) toggled_ = !toggled_;
    } else {
        toggled_ = false;
    }
    lastSustain_ = sus;
    bool fast = (mode == Fast) != toggled_;
    if (p.get(ModWheel) >= 0.5f && ch && ch->modWheel >= 0.5f) fast = true;
    const bool stop = mode == Stop;
    const float hornTarget = stop ? 0.0f : p.get(fast ? HornFastRpm : HornSlowRpm);
    const float drumTarget = stop ? 0.0f : p.get(fast ? DrumFastRpm : DrumSlowRpm);
    const float inertia = p.get(Inertia);
    const double sr = sampleRate_;
    auto coef = [&](float tau) { return dsp::onePoleCoef(tau * inertia, sr); };
    const float hornCoef = coef(hornTarget > horn_.rpm ? kHornAccel : kHornDecel);
    const float drumCoef = coef(drumTarget > drum_.rpm ? kDrumAccel : kDrumDecel);
    const float radPerRpm = dsp::kTwoPi / 60.0f / static_cast<float>(sr);

    const float drive = p.get(Drive);
    preamp_.setDrive(drive);
    mix_.setTarget(p.get(Mix));
    level_.setTarget(dsp::dbToGain(p.get(LevelDb)));
    spread_.setTarget(p.get(Spread));
    balance_.setTarget(p.get(Balance));

    for (int i = 0; i < io.numSamples; ++i) {
        const float dryL = io.left[i], dryR = io.right[i];
        float x = 0.5f * (dryL + dryR);
        x = preamp_.process(x); // always on: no path switching when drive is automated
        float lo, hi;
        xover_.process(x, lo, hi);
        hi = hornCab_.tick(hi).lp;
        lo += 0.3f * drumCab_.tick(lo).bp;
        hornLine_.write(hi);
        drumLine_.write(lo);

        horn_.rpm += (hornTarget - horn_.rpm) * hornCoef;
        drum_.rpm += (drumTarget - drum_.rpm) * drumCoef;
        if (std::fabs(hornTarget - horn_.rpm) < 1e-4f) horn_.rpm = hornTarget;
        if (std::fabs(drumTarget - drum_.rpm) < 1e-4f) drum_.rpm = drumTarget;
        horn_.angle += horn_.rpm * radPerRpm;
        if (horn_.angle >= dsp::kTwoPi) horn_.angle -= dsp::kTwoPi;
        drum_.angle -= drum_.rpm * radPerRpm; // drum turns the other way
        if (drum_.angle < 0.0f) drum_.angle += dsp::kTwoPi;

        const float spread = spread_.next();
        const float bal = balance_.next();
        const float hg = std::min(1.0f, 1.0f + bal), dg = std::min(1.0f, 1.0f - bal);
        float wet[2];
        for (int m = 0; m < 2; ++m) {
            const float micAngle = (m == 0 ? -0.5f : 0.5f) * dsp::kPi * spread;
            // Horn, direct path.
            const float c = std::cos(horn_.angle - micAngle);
            const float facing = 0.5f + 0.5f * c;
            const float s = hornLine_.read(1.0f + hornDepth_ * (1.0f - c));
            dirLp_[m] += (s - dirLp_[m]) * (lpAway_ + (lpFacing_ - lpAway_) * facing);
            float h = dirLp_[m] * (1.0f - kHornAm * (1.0f - facing));
            // Horn, cabinet reflection (mouth pointing away from this mic).
            const float s2 = hornLine_.read(1.0f + reflDelay_ + hornDepth_ * (1.0f + c));
            reflLp_[m] += (s2 - reflLp_[m]) * lpAway_;
            h += kReflGain * reflLp_[m] * (1.0f - kHornAm * facing);
            // Drum (bass rotor), narrower mic spread.
            const float cd = std::cos(drum_.angle - 0.6f * micAngle);
            const float d = drumLine_.read(1.0f + drumDepth_ * (1.0f - cd)) * (1.0f - kDrumAm * (0.5f - 0.5f * cd));
            wet[m] = h * hg + d * dg;
        }
        const float mix = mix_.next(), lvl = level_.next();
        io.left[i] = (dryL * (1.0f - mix) + wet[0] * mix) * lvl;
        io.right[i] = (dryR * (1.0f - mix) + wet[1] * mix) * lvl;
    }
    params().setRaw(HornRpm, horn_.rpm);
    params().setRaw(DrumRpm, drum_.rpm);
}

} // namespace ks
