#pragma once
// `wah`: inductor wah pedal (Dunlop Cry Baby / Vox V846). Resonant band-pass (TPT SVF, unit gain at the centre)
// plus a low-passed "body" path, swept exponentially between the model's heel and toe frequencies:
//   Cry Baby  ~350 Hz .. 2.2 kHz, Q rises towards the toe;   Vox  ~450 Hz .. 2.6 kHz, flatter, lower Q.
// Pedal position 0 (heel) .. 1 (toe) comes from
//   mode Manual: `pedal`, or the layer's mod wheel / CC11 expression (`pedal_source`),
//   mode Auto:   `pedal` + `sens` x input envelope (attack_ms / release_ms),
//   mode LFO:    lerp(`pedal`, LFO, `depth`), LFO at `rate` Hz or `sync` (phase locked to the transport).
// `range` scales the sweep around its geometric centre (1 = model, < 1 narrower), `q` the resonance.
// Zero latency; the tail is the band-pass ring (worst case: high Q at the heel, ~0.3 s to -60 dB).

#include "core/Module.h"
#include "dsp/EnvelopeFollower.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"
#include "dsp/TptFilters.h"

namespace ks {

class WahFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    WahFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Mode, Pedal, PedalSource, Model, Range, Q, Sens, AttackMs, ReleaseMs, Rate, Sync, Depth, LevelDb, Mix, Count };
    enum ModeId { Manual, Auto, LfoMode };
    enum SourceId { SrcParam, SrcModWheel, SrcExpression };
    enum ModelId { CryBaby, Vox };

    // Centre frequency (Hz) of the band-pass at pedal position 0..1 for the current model/range (tests).
    float centreHz(float position) const noexcept;
    // Pedal position used for the last processed sample (tests / meters).
    float lastPosition() const noexcept { return pos_.value(); }

private:
    double sr_ = 48000.0;
    dsp::Svf svf_[2];
    dsp::TptOnePole body_[2];
    dsp::EnvelopeFollower env_;
    dsp::OnePoleSmoother pos_, mix_, level_;
    double phase_ = 0.0;
};

} // namespace ks
