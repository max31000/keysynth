#pragma once
// `delay`: stereo / ping-pong / tape echo. Zero latency (the dry path is untouched; echoes start at `time`).
//   time      ms (1..2000), or `sync` note division at the transport tempo (1/8. = Floyd "Run Like Hell" /
//             "Another Brick" dotted eighth). Up to kMaxSeconds. Time changes glide (varispeed pitch bend).
//   feedback  0..1 (1 = infinite repeats; a soft clipper in the loop keeps it bounded),
//   low_cut_hz / high_cut_hz  filters inside the feedback loop (every repeat darker / thinner),
//   offset    right-channel time offset in % (stereo), width, duck (wet ducks under the input), mix.
//   Tape mode additionally applies wow, flutter and drive (tape saturation in the loop); the other modes ignore
//   them. Ping-pong feeds the mono input into the left line and cross-feeds the lines.

#include "core/Module.h"
#include "dsp/InterpDelay.h"
#include "dsp/ModLfo.h"
#include "dsp/Smoother.h"
#include "dsp/TptFilters.h"

#include <atomic>

namespace ks {

class DelayFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    DelayFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Mode, Time, Sync, Feedback, Mix, LowCutHz, HighCutHz, Offset, Width, Duck, Wow, Flutter, Drive, Count };
    enum ModeId { Stereo, PingPong, Tape };
    static constexpr double kMaxSeconds = 5.0;

    // Current target delay of the left line in samples (tests).
    float targetDelaySamples(const ProcessContext& ctx) const noexcept;

private:
    struct Channel {
        dsp::InterpDelay line;
        dsp::TptOnePole hp;
        dsp::TptSvf lp;
        dsp::OnePoleSmoother delay;
    };
    double sr_ = 48000.0;
    Channel ch_[2];
    dsp::Lfo wow_, flutter_;
    dsp::OnePoleSmoother fb_, mix_, width_, duck_, wowAmt_, flutAmt_, drive_;
    float env_ = 0.0f, envAtt_ = 0.0f, envRel_ = 0.0f;
    float lastLow_ = -1.0f, lastHigh_ = -1.0f;
    std::atomic<double> lastTempo_{120.0}; // audio thread writes; tailSamples() may read from any thread
};

} // namespace ks
