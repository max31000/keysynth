#pragma once
// `tremolo`: amp tremolo (Wurlitzer / Fender style amplitude modulation) and Rhodes Suitcase-style stereo
// vibrato (autopan). LFO: sine / triangle / square with adjustable edge smoothing; free rate or tempo sync
// (phase-locked to the transport ppq position while it plays). Tremolo mode: gain = 1 - depth * (1 - lfo)/2 per
// channel, with an adjustable L/R phase offset. Autopan mode: equal-power balance, L *= sqrt(1 - p),
// R *= sqrt(1 + p), p = depth * lfo (a centred source sweeps between the speakers). Zero latency, no allocation.

#include "core/Module.h"
#include "dsp/Smoother.h"

namespace ks {

class TremoloFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    TremoloFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;

    enum P { Mode, Rate, Sync, Depth, Shape, Smoothing, StereoPhase, Count };
    enum class ModeId { Tremolo = 0, Autopan = 1 };
    enum class ShapeId { Sine = 0, Triangle = 1, Square = 2 };

    // LFO value in [-1, 1] for phase in [0, 1) (shape, smoothing 0..1 for the square).
    static float lfoValue(double phase, ShapeId shape, float smoothing) noexcept;

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0; // 0..1
    dsp::OnePoleSmoother depth_;
};

} // namespace ks
