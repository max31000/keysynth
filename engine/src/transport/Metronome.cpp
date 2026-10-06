#include "transport/Metronome.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

void Metronome::setVolume(float v) noexcept {
    if (std::isfinite(v)) volume_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Metronome::prepare(double sampleRate) noexcept {
    sampleRate_ = sampleRate;
    decay_ = std::exp(-1.0f / (0.012f * static_cast<float>(sampleRate))); // ~12 ms time constant
    reset();
}

void Metronome::trigger(bool accent) noexcept {
    const float hz = accent ? 1760.0f : 1320.0f;
    inc_ = hz / static_cast<float>(sampleRate_);
    phase_ = 0.0f;
    env_ = 1.0f;
    amp_ = (accent ? 0.5f : 0.35f) * volume_.load(std::memory_order_relaxed);
    remaining_ = static_cast<int>(0.08 * sampleRate_);
}

float Metronome::nextSample() noexcept {
    if (remaining_ <= 0) return 0.0f;
    --remaining_;
    const float s = std::sin(dsp::kTwoPi * phase_) * env_ * amp_;
    phase_ += inc_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    env_ *= decay_;
    return s;
}

void Metronome::process(const AudioBlock& out, const TransportInfo& t, bool started, double sampleRate) noexcept {
    const bool on = enabled_.load(std::memory_order_relaxed) && t.playing;
    const int n = out.numSamples;
    if (!on && remaining_ <= 0) return;
    const double beatsPerSample = t.tempo / (60.0 * sampleRate);
    const double b0 = t.ppqPosition;
    // Next beat index at or after the block start.
    double nextBeat = started ? 0.0 : std::ceil(b0 - 1e-9);
    int i = 0;
    while (i < n) {
        int clickAt = n;
        if (on) {
            const double off = (nextBeat - b0) / beatsPerSample;
            if (off < static_cast<double>(n)) clickAt = std::max(i, static_cast<int>(std::max(0.0, std::floor(off))));
        }
        for (; i < clickAt; ++i) {
            const float s = nextSample();
            out.left[i] += s;
            out.right[i] += s;
        }
        if (clickAt < n) {
            const int beat = static_cast<int>(std::llround(nextBeat));
            trigger(t.numerator > 0 && beat % t.numerator == 0);
            nextBeat += 1.0;
        }
    }
}

} // namespace ks
