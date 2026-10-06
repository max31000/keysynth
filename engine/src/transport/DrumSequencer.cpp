#include "transport/DrumSequencer.h"

#include <algorithm>
#include <cmath>

namespace ks {

DrumSequencer::DrumSequencer() {
    // All three slots start as the empty 4/4 16-step pattern (numTracks = 0).
}

void DrumSequencer::setPattern(const RtPattern& p) noexcept {
    buf_[static_cast<size_t>(back_)] = p;
    const int old = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel);
    back_ = old & 3;
}

void DrumSequencer::syncPattern() noexcept {
    if ((middle_.load(std::memory_order_acquire) & kDirty) == 0) return;
    const double oldStep = buf_[static_cast<size_t>(front_)].stepPpq();
    const int old = middle_.exchange(front_, std::memory_order_acq_rel);
    front_ = old & 3;
    const double newStep = buf_[static_cast<size_t>(front_)].stepPpq();
    if (running_ && newStep != oldStep) {
        // Different step length: re-anchor at the next (unfired) step; the new pattern starts at its step 0 there.
        anchorPpq_ += static_cast<double>(nextStep_ - anchorStep_) * oldStep;
        anchorStep_ = nextStep_;
    }
}

void DrumSequencer::start(double originPpq) noexcept {
    syncPattern();
    running_ = true;
    nextStep_ = 0;
    anchorStep_ = 0;
    anchorPpq_ = originPpq;
    currentStep_.store(-1, std::memory_order_relaxed);
}

void DrumSequencer::stop() noexcept {
    running_ = false;
    currentStep_.store(-1, std::memory_order_relaxed);
}

int DrumSequencer::generate(const TransportInfo& t, int numSamples, double sampleRate, float swing, bool emit,
                            MidiEvent* out, int cap) noexcept {
    syncPattern();
    if (!running_ || !t.playing || numSamples <= 0 || t.tempo <= 0.0) return 0;
    const RtPattern& p = buf_[static_cast<size_t>(front_)];
    const double stepPpq = p.stepPpq();
    const double samplesPerPpq = 60.0 * sampleRate / t.tempo;
    const double sw = std::clamp(static_cast<double>(swing), 0.0, 1.0) * 0.5 * stepPpq;
    constexpr double kEps = 1e-4; // samples
    int count = 0;
    for (int guard = 0; guard < kMaxStepsPerBlock; ++guard) {
        const int64_t rel = nextStep_ - anchorStep_;
        const int idx = p.numSteps > 0 ? static_cast<int>(rel % p.numSteps) : static_cast<int>(rel & 1);
        // Swing by position inside the pattern (stable for odd step counts).
        const double ppq = anchorPpq_ + static_cast<double>(rel) * stepPpq + ((idx & 1) ? sw : 0.0);
        const double off = (ppq - t.ppqPosition) * samplesPerPpq;
        // First sample at or after the exact step time (late steps, e.g. after a tempo jump, fire at 0).
        if (off - kEps > static_cast<double>(numSamples - 1)) break;
        const int at = off <= kEps ? 0 : static_cast<int>(std::ceil(off - kEps));
        const uint32_t offset = static_cast<uint32_t>(std::min(at, numSamples - 1));
        if (p.numSteps > 0) {
            currentStep_.store(idx, std::memory_order_relaxed);
            if (emit) {
                for (int k = 0; k < p.numTracks && count < cap; ++k) {
                    const RtPattern::Track& tr = p.tracks[static_cast<size_t>(k)];
                    const uint8_t v = tr.vel[static_cast<size_t>(idx)];
                    if (v == 0 || tr.mute) continue;
                    out[count++] = MidiEvent::noteOn(tr.note, v, kChannel, offset);
                }
            }
        }
        ++nextStep_;
    }
    return count;
}

} // namespace ks
