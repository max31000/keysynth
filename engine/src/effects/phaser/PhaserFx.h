#pragma once
// `phaser`: analog-style all-pass phaser (Electro-Harmonix Small Stone / MXR Phase 90; 12 stages for the lush
// string-machine/"Eventide" sweep). N first-order TPT all-passes with one shared, exponentially swept corner,
// regenerative feedback ("color"), dry/wet mix (0.5 = classic full-depth notches, N/2 of them), stereo spread
// via an LFO phase offset on the right channel, optional tempo sync. Zero latency.

#include "core/Module.h"
#include "dsp/ModLfo.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

#include <array>

namespace ks {

class PhaserFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    PhaserFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Stages, Rate, Sync, Depth, CentreHz, Feedback, Spread, Mix, Count };
    static constexpr int kMaxStages = 12;
    static int stageCount(int choice) noexcept;

private:
    struct Channel {
        std::array<dsp::TptOnePole, kMaxStages> ap;
        float last = 0.0f;
    };
    double sr_ = 48000.0;
    Channel ch_[2];
    dsp::Lfo lfo_;
    dsp::OnePoleSmoother depth_, centre_, fb_, spread_, mix_;
    int lastStages_ = -1;
};

} // namespace ks
