#pragma once
// Synthesized metronome click (ARCHITECTURE §8): a short decaying sine burst on every beat, higher-pitched
// accent on the downbeat. Mixed by the Engine after master FX, before the safety limiter. RT-safe.

#include "core/AudioBlock.h"
#include "core/ProcessContext.h"

#include <atomic>

namespace ks {

class Metronome {
public:
    // control thread
    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
    void setVolume(float v01) noexcept;
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    float volume() const noexcept { return volume_.load(std::memory_order_relaxed); }

    void prepare(double sampleRate) noexcept;
    void reset() noexcept { remaining_ = 0; }

    // audio thread: adds clicks for beats that fall inside this block. `started` = transport just started.
    void process(const AudioBlock& out, const TransportInfo& t, bool started, double sampleRate) noexcept;

private:
    void trigger(bool accent) noexcept;
    float nextSample() noexcept;

    std::atomic<bool> enabled_{false};
    std::atomic<float> volume_{0.5f};
    double sampleRate_ = 48000.0;
    // voice
    int remaining_ = 0;
    float phase_ = 0.0f, inc_ = 0.0f, env_ = 0.0f, decay_ = 0.999f, amp_ = 0.0f;
};

} // namespace ks
