#pragma once
// `compressor`: stereo-linked, zero-lookahead, feed-forward log-domain compressor.
//   VCA  (SSL-bus style): peak detector, soft-knee gain computer, branching attack/release smoothing of the
//        gain in dB (Giannoulis/Massberg/Reiss "smooth decoupled"), user attack/release.
//   Opto (LA-2A style): fixed ~10 ms attack and a two-stage program-dependent release (fast ~60 ms part plus a
//        slow part that charges while compression is sustained, 0.5..5 s); `attack_ms` is ignored and
//        `release_ms` scales the slow stage. A wide minimum knee (6 dB) gives the gentle opto curve.
// `sc_hpf_hz` high-passes the detector only. `mix` = parallel (New York) compression. ReadOnly `gr_db` = current
// gain reduction (positive dB, peak over the last block) for the UI meter.

#include "core/Module.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

namespace ks {

class CompressorFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    CompressorFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;

    enum P { Mode, ThresholdDb, Ratio, AttackMs, ReleaseMs, KneeDb, MakeupDb, Mix, ScHpfHz, GainReductionDb, Count };
    enum ModeId { Vca, Opto };

    // Static gain computer: gain (dB, <= 0) applied to a detector level `levelDb`.
    static float staticGainDb(float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept;

private:
    double sr_ = 48000.0;
    dsp::TptOnePole scHp_[2];
    float gsDb_ = 0.0f;               // VCA smoothed gain (dB, <= 0)
    float fastDb_ = 0.0f, slowDb_ = 0.0f; // opto stages
    dsp::OnePoleSmoother makeup_, mix_;
    float lastHpf_ = -1.0f;
};

} // namespace ks
