#pragma once
// `drive`: tube overdrive / fuzz / tape saturation. The nonlinearity runs 2x or 4x oversampled through
// polyphase IIR half-band filters (dsp::Oversampler: min-phase-style, no lookahead, latencySamples() == 0).
//   drive_db   input gain into the shaper,  asymmetry  bias -> even harmonics (DC removed afterwards),
//   tone       post tilt EQ (0 dark .. 0.5 flat .. 1 bright, +-6 dB around 800 Hz),
//   level_db   output level, mix  parallel blend. The dry path runs through an identical (unshaped)
//   up/down filter pair so the blend stays phase-coherent (mix 0 = all-pass filtered input, flat magnitude).
// Modes: Tube (asymmetric soft clip, gentle), Fuzz (hard, high gain), Tape (soft tanh with HF pre/de-emphasis,
// i.e. treble compresses first).

#include "core/Module.h"
#include "dsp/HalfbandIir.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

namespace ks {

class DriveFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    DriveFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Mode, DriveDb, Asymmetry, Tone, LevelDb, Mix, Oversample, Count };
    enum ModeId { Tube, Fuzz, Tape };

    static float shape(int mode, float x, float bias) noexcept;

private:
    struct Channel {
        dsp::Oversampler os, osDry;
        dsp::TptOnePole preEmph, deEmph; // tape mode
        dsp::TptOnePole tilt;
        dsp::DcBlocker dc;
    };
    double sr_ = 48000.0;
    int maxBlock_ = 0;
    Channel ch_[2];
    std::vector<float> dry_, tmp_; // dry: maxBlock, tmp: 4 x maxBlock control lanes
    dsp::OnePoleSmoother drive_, bias_, level_, mix_, tone_;
    int lastFactor_ = 0;
};

} // namespace ks
