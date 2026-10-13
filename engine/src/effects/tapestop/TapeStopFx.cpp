#include "effects/tapestop/TapeStopFx.h"

#include <algorithm>
#include <cmath>

namespace ks {

namespace {
constexpr float kFadeSpeed = 0.04f; // below 4 % speed the level fades to 0 (smoothstep)

float fadeGain(float v) noexcept {
    const float x = std::clamp(v / kFadeSpeed, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
} // namespace

const ModuleInfo& TapeStopFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "tapestop";
        i.displayName = "Tape Stop";
        i.kind = ModuleKind::Effect;
        i.category = "Modulation";
        auto speed = linearParam("speed", "Speed", 0.0f, 1.0f, 1.0f);
        speed.flags = ParamFlags::ReadOnly;
        i.params = {
            boolParam("stop", "Stop", false),
            enumParam("trigger", "Trigger", {"Param", "MIDI Note", "MIDI CC"}, TrigParam, "Trigger"),
            intParam("trigger_note", "Trigger Note", 0, 127, 24, {}, "Trigger"),
            intParam("trigger_cc", "Trigger CC", 0, 119, 80, {}, "Trigger"),
            logParam("stop_ms", "Stop Time", 20.0f, 4000.0f, 700.0f, "ms", {}, 500.0f),
            logParam("start_ms", "Start Time", 20.0f, 4000.0f, 350.0f, "ms", {}, 500.0f),
            linearParam("curve", "Curve", -1.0f, 1.0f, 0.0f),
            linearParam("mix", "Mix", 0.0f, 1.0f, 1.0f),
            speed,
        };
        i.uiHints = {{"front", {"stop", "stop_ms", "start_ms", "curve", "trigger", "speed"}}};
        return i;
    }();
    return info;
}

TapeStopFx::TapeStopFx() : Module(moduleInfo()) {}

void TapeStopFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    // Longest delay: a full stop ramp (integral of 1 - v <= stop time) or a full start ramp from standstill.
    for (auto& l : line_) l.prepare(static_cast<int>(kMaxRampSeconds * sampleRate) + 64);
    mix_.prepare(sampleRate, 0.02f);
    gain_.prepare(sampleRate, 0.002f);
    reset();
}

void TapeStopFx::reset() {
    for (auto& l : line_) l.reset();
    mix_.snap(params().get(Mix));
    gain_.snap(1.0f);
    midiStop_ = false;
    state_ = State::Running;
    v_ = v0_ = 1.0f;
    d_ = 0.0;
    rampLen_ = 1;
    rampPos_ = 0;
    xfadeLen_ = std::max(1, static_cast<int>(kCrossfadeSeconds * sr_));
    xfadePos_ = 0;
    params().setRaw(Speed, 1.0f);
}

int TapeStopFx::tailSamples() const {
    // A stop or start ramp in flight keeps producing (resampled) audio for at most its length, then the crossfade.
    const double s = (params().get(StopMs) + params().get(StartMs)) * 0.001 + kCrossfadeSeconds + 0.01;
    return static_cast<int>(s * sr_);
}

float TapeStopFx::shape(float u) const noexcept { return std::pow(std::clamp(u, 0.0f, 1.0f), curveExp_); }

void TapeStopFx::beginStop() noexcept {
    v0_ = v_;
    if (v0_ <= 1e-4f) {
        v_ = 0.0f;
        state_ = State::Stopped;
        return;
    }
    const double len = std::clamp(static_cast<double>(params().get(StopMs)), 20.0, 4000.0) * 0.001 * sr_ * v0_;
    rampLen_ = std::max(1, static_cast<int>(len));
    rampPos_ = 0;
    state_ = State::Stopping;
}

void TapeStopFx::beginStart() noexcept {
    if (state_ == State::Stopped) { // from standstill: the read head restarts at the write head (silent here)
        v_ = 0.0f;
        d_ = 0.0;
    }
    v0_ = v_;
    const double len = std::clamp(static_cast<double>(params().get(StartMs)), 20.0, 4000.0) * 0.001 * sr_ * (1.0 - v0_);
    rampLen_ = std::max(1, static_cast<int>(len));
    rampPos_ = 0;
    state_ = State::Starting;
}

void TapeStopFx::process(AudioBlock& io, MidiEventSpan events, const ProcessContext&) {
    const ParamSet& p = params();
    const int trig = std::clamp(static_cast<int>(p.get(Trigger)), 0, 2);
    const int note = static_cast<int>(p.get(TriggerNote));
    const int cc = static_cast<int>(p.get(TriggerCc));
    const bool paramStop = p.get(Stop) >= 0.5f;
    curveExp_ = std::exp2(2.0f * std::clamp(p.get(Curve), -1.0f, 1.0f));
    mix_.setTarget(p.get(Mix));
    const double maxD = static_cast<double>(line_[0].maxDelay()) - 4.0;
    size_t ev = 0;

    for (int i = 0; i < io.numSamples; ++i) {
        // Sample-accurate MIDI triggers.
        for (; ev < events.size() && static_cast<int>(events[ev].sampleOffset) <= i; ++ev) {
            const MidiEvent& e = events[ev];
            if (trig == TrigNote && e.isNote() && e.data1 == note)
                midiStop_ = e.type == MidiEventType::NoteOn && e.value7 > 0;
            else if (trig == TrigCc && e.type == MidiEventType::ControlChange && e.data1 == cc)
                midiStop_ = e.value7 >= 64;
            else if (e.type == MidiEventType::AllSoundOff)
                midiStop_ = false;
        }
        const bool want = trig == TrigParam ? paramStop : midiStop_;
        if (want && (state_ == State::Running || state_ == State::Starting)) beginStop();
        else if (!want && (state_ == State::Stopping || state_ == State::Stopped)) beginStart();
        // (a stop during the 10 ms crossfade waits for its end: no jump between the blended paths)

        const float xl = io.left[i], xr = io.right[i];
        line_[0].push(xl);
        line_[1].push(xr);

        switch (state_) {
        case State::Stopping:
            ++rampPos_;
            v_ = v0_ * (1.0f - shape(static_cast<float>(rampPos_) / static_cast<float>(rampLen_)));
            if (rampPos_ >= rampLen_) {
                v_ = 0.0f;
                state_ = State::Stopped;
            }
            break;
        case State::Starting:
            ++rampPos_;
            v_ = v0_ + (1.0f - v0_) * shape(static_cast<float>(rampPos_) / static_cast<float>(rampLen_));
            if (rampPos_ >= rampLen_) {
                v_ = 1.0f;
                if (d_ > 1e-3) {
                    state_ = State::Crossfade;
                    xfadePos_ = 0;
                } else {
                    state_ = State::Running;
                    d_ = 0.0;
                }
            }
            break;
        default: break;
        }
        if (state_ == State::Stopping || state_ == State::Starting || state_ == State::Stopped)
            d_ += 1.0 - static_cast<double>(v_);
        if (d_ >= maxD) { // stop/start mashed for seconds without ever finishing: out of tape -> stand still
            d_ = maxD;
            if (state_ != State::Stopped) {
                v_ = 0.0f;
                state_ = State::Stopped; // gain_ fades out over 2 ms; the next start begins fresh
            }
        }

        gain_.setTarget(state_ == State::Stopped ? 0.0f : fadeGain(v_));
        const float g = gain_.next();
        float yl = xl, yr = xr;
        if (state_ == State::Stopped && g == 0.0f) {
            yl = yr = 0.0f;
        } else if (state_ != State::Running) {
            const float d = static_cast<float>(d_);
            float rl, rr;
            if (d < 1.0f) {
                rl = line_[0].readLinear(d);
                rr = line_[1].readLinear(d);
            } else {
                rl = line_[0].readHermite(d);
                rr = line_[1].readHermite(d);
            }
            if (state_ == State::Crossfade) {
                const float a = static_cast<float>(xfadePos_) / static_cast<float>(xfadeLen_);
                yl = rl + (xl - rl) * a;
                yr = rr + (xr - rr) * a;
                if (++xfadePos_ >= xfadeLen_) {
                    state_ = State::Running;
                    d_ = 0.0;
                }
            } else {
                yl = rl * g;
                yr = rr * g;
            }
        }
        const float mx = mix_.next();
        io.left[i] = xl + (yl - xl) * mx;
        io.right[i] = xr + (yr - xr) * mx;
    }
    params().setRaw(Speed, v_);
}

} // namespace ks
