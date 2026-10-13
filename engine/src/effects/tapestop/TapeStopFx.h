#pragma once
// `tapestop`: tape stop / start. The input is written to a circular buffer every sample; a read head moves at the
// tape speed v (1 = normal). While running (v = 1, no residue) the output is the dry input: zero latency.
//   Stop (the `stop` param on, or the trigger note held / trigger CC >= 64 when `trigger` is MIDI Note / MIDI CC):
//   v ramps from its current value to 0 over `stop_ms` (scaled by the current v), the read head falls behind the
//   write head (fractional Hermite read = smooth resampling, pitch follows v), level fades out over the last few
//   percent of speed (no DC hold, no click). Stopped: silence.
//   Start: v ramps up to 1 over `start_ms`. From standstill the read head restarts at the write head (silent there,
//   so no click); once v is 1 again the accumulated delay is removed by a 10 ms crossfade to the dry input.
//   `curve` shapes both ramps (progress u^e, e = 4^curve): 0 linear; > 0 the change comes late (stop: keeps speed,
//   then drops; start: crawls, then jumps up), < 0 early (stop: drops fast, then crawls to a halt).
// Trigger notes/CCs still reach the instrument before this effect (pick a key outside the playing range or a CC).
// ReadOnly `speed` shows v.

#include "core/Module.h"
#include "dsp/InterpDelay.h"
#include "dsp/Smoother.h"

namespace ks {

class TapeStopFx final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    TapeStopFx();
    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& io, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;

    enum P { Stop, Trigger, TriggerNote, TriggerCc, StopMs, StartMs, Curve, Mix, Speed, Count };
    enum TriggerId { TrigParam, TrigNote, TrigCc };
    static constexpr double kMaxRampSeconds = 4.0;
    static constexpr double kCrossfadeSeconds = 0.010;

    float speed() const noexcept { return v_; }       // tests
    bool running() const noexcept { return state_ == State::Running; }

private:
    enum class State { Running, Stopping, Stopped, Starting, Crossfade };
    void beginStop() noexcept;
    void beginStart() noexcept;
    float shape(float u) const noexcept;

    double sr_ = 48000.0;
    dsp::InterpDelay line_[2];
    dsp::OnePoleSmoother mix_;
    dsp::OnePoleSmoother gain_; // level fade near standstill, slew-limited (a steep curve may jump v)
    State state_ = State::Running;
    float v_ = 1.0f;         // tape speed
    float v0_ = 1.0f;        // speed at the start of the current ramp
    double d_ = 0.0;         // read-head delay behind the write head (samples)
    int rampLen_ = 1, rampPos_ = 0;
    int xfadeLen_ = 1, xfadePos_ = 0;
    float curveExp_ = 1.0f;
    bool midiStop_ = false;  // trigger note held / CC >= 64
};

} // namespace ks
