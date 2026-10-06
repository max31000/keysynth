#pragma once
// `rotary`: Leslie 122/147 rotary speaker (ARCHITECTURE §7).
//   mono in -> 122 preamp (tube drive) -> LR4 crossover 800 Hz -> horn (treble) / drum (bass) rotors.
//   Each rotor: angle integrates rpm; rpm approaches the target with separate accel/decel time constants
//   (horn ~0.16 s / 0.32 s, drum ~4.1 s / 1.4 s, scaled by `inertia`). Per mic (L/R at +-spread): Doppler =
//   modulated delay (horn r ~ 0.15 m -> +-0.44 ms), amplitude modulation and a directional low-pass (horn
//   facing away = duller); the horn also has a cabinet reflection path (opposite side, later, quieter).
//   Speed: param Slow/Fast/Stop (brake), optionally mod wheel >= 64 = fast, optionally sustain pedal toggles.
//   Read-only `horn_rpm` / `drum_rpm` for the UI.

#include "core/Module.h"
#include "dsp/InterpDelay.h"
#include "dsp/LR4Crossover.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"
#include "dsp/TubeStage.h"

namespace ks {

class RotaryFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    RotaryFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override { return static_cast<int>(0.02 * sampleRate_); }

    enum P {
        Speed, ModWheel, SustainToggle, Drive, Balance, Spread,
        HornSlowRpm, HornFastRpm, DrumSlowRpm, DrumFastRpm, Inertia, Mix, LevelDb,
        HornRpm, DrumRpm,
        Count
    };
    enum SpeedMode { Slow = 0, Fast = 1, Stop = 2 };

    // Base time constants (s) at inertia 1.
    static constexpr float kHornAccel = 0.161f, kHornDecel = 0.321f;
    static constexpr float kDrumAccel = 4.127f, kDrumDecel = 1.371f;

    float hornRpm() const noexcept { return horn_.rpm; }
    float drumRpm() const noexcept { return drum_.rpm; }

private:
    struct Rotor {
        float rpm = 0.0f;
        float angle = 0.0f; // radians
    };

    double sampleRate_ = 48000.0;
    Rotor horn_, drum_;
    dsp::TubeStage preamp_;
    dsp::LR4Crossover xover_;
    dsp::Svf hornCab_, drumCab_;
    dsp::InterpDelay hornLine_, drumLine_;
    float dirLp_[2] = {}, reflLp_[2] = {}; // directional one-pole states per mic
    dsp::OnePoleSmoother mix_, level_, spread_, balance_;
    bool lastSustain_ = false;
    bool toggled_ = false;
    float hornDepth_ = 20.0f, drumDepth_ = 6.0f, reflDelay_ = 40.0f; // samples
    float lpFacing_ = 0.9f, lpAway_ = 0.25f;                        // one-pole coefs
};

} // namespace ks
