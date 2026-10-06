#pragma once
// `eq`: HP + low shelf + 3 parametric bells + high shelf + LP, stereo. Simper TPT SVFs (RBJ-equivalent
// responses, stable under modulation); parameters are smoothed and coefficients refreshed every 16 samples.
// HP/LP are 12 or 24 dB/oct Butterworth (`cut_slope`). Flat bands (0 dB) and disabled cuts are skipped, so a flat
// EQ is bit-transparent. Zero latency.

#include "core/Module.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

#include <array>

namespace ks {

class EqFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    EqFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P {
        HpOn, HpHz, LpOn, LpHz, CutSlope,
        LsHz, LsDb,
        P1Hz, P1Db, P1Q,
        P2Hz, P2Db, P2Q,
        P3Hz, P3Db, P3Q,
        HsHz, HsDb,
        LevelDb, Count
    };
    static constexpr int kSub = 16; // coefficient update interval

private:
    enum BandId { Hp, Ls, P1, P2, P3, Hs, Lp, NumBands };
    struct Band {
        dsp::TptSvf f[2][2]; // [stage][channel]; stage 1 only for 24 dB cuts
        dsp::OnePoleSmoother hz, db, q;
        bool active = false;
        int stages = 0;
    };
    void configure(Band& b, BandId id, float sr) noexcept;

    double sr_ = 48000.0;
    std::array<Band, NumBands> bands_;
    dsp::OnePoleSmoother level_;
};

} // namespace ks
