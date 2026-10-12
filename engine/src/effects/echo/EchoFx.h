#pragma once
// `echo`: Binson Echorec magnetic drum echo (Pink Floyd: "One of These Days", "Echoes", "Time"). One record head
// and 4 playback heads equally spaced around the drum: head k plays at k x `time` (head spacing, ms, or the `sync`
// division at the transport tempo). `heads` is the selector switch (12 combinations of the 4 heads); the selected
// heads give the rhythmic multi-tap pattern and also feed the regeneration (`feedback`, "Swell"; > ~0.9 runs away
// into the Echorec's self-oscillating swell, bounded by the record-amp saturation). `tone` is the loop low-pass,
// `drive` the valve record amplifier, `wow` / `flutter` modulate the drum speed (every head by k x the same
// relative amount), `width` pans heads 1/3 left and 2/4 right. Mono drum (input summed), zero latency (the dry path
// is untouched), Hermite reads from dsp::InterpDelay.

#include "core/Module.h"
#include "dsp/InterpDelay.h"
#include "dsp/ModLfo.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

#include <array>
#include <atomic>

namespace ks {

class EchoFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    EchoFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Heads, Time, Sync, Feedback, Tone, Drive, Wow, Flutter, Width, Mix, Count };
    static constexpr int kNumCombos = 12;
    static constexpr double kMaxSpacingSeconds = 1.0; // head 4 at most 4 s behind the record head

    // Bit mask (bit k-1 = head k) of a `heads` index.
    static unsigned headMask(int headsIndex) noexcept;
    // Head spacing in samples for the current params at `tempo` (tests).
    float spacingSamples(double tempo) const noexcept;

private:
    double sr_ = 48000.0;
    dsp::InterpDelay drum_;
    dsp::TptOnePole loopHp_;
    dsp::TptSvf loopLp_;
    dsp::Lfo wow_, flutter_;
    // Head spacing glide (drum motor) in double precision: a float one-pole stalls a few samples short of a
    // target of thousands of samples and then snaps, which would jump every head (click).
    double spacing_ = 4800.0, spacingTarget_ = 4800.0, spacingCoef_ = 1.0;
    dsp::OnePoleSmoother fb_, mix_, width_, wowAmt_, flutAmt_, drive_;
    std::array<dsp::OnePoleSmoother, 4> headGain_; // selector changes fade (no clicks)
    float lastTone_ = -1.0f;
    std::atomic<double> lastTempo_{120.0}; // audio thread writes; tailSamples() may read from any thread
};

} // namespace ks
