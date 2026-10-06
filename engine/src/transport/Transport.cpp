#include "transport/Transport.h"

#include <algorithm>
#include <cmath>

namespace ks {

void Transport::setTempo(double bpm) noexcept {
    if (std::isfinite(bpm)) tempo_.store(std::clamp(bpm, 20.0, 400.0), std::memory_order_relaxed);
}

void Transport::setTimeSignature(int num, int den) noexcept {
    num_.store(std::clamp(num, 1, 32), std::memory_order_relaxed);
    den_.store(den == 2 || den == 4 || den == 8 || den == 16 ? den : 4, std::memory_order_relaxed);
}

void Transport::resetPosition() noexcept {
    ppq_ = anchorPpq_ = 0.0;
    sinceAnchor_ = 0;
}

TransportInfo Transport::beginBlock(bool& started, bool& stopped) noexcept {
    blockPlaying_ = playing_.load(std::memory_order_relaxed);
    blockTempo_ = tempo_.load(std::memory_order_relaxed);
    started = blockPlaying_ && !wasPlaying_;
    stopped = !blockPlaying_ && wasPlaying_;
    if (started) resetPosition();
    if (blockTempo_ != anchorTempo_) {
        anchorPpq_ = ppq_;
        sinceAnchor_ = 0;
        anchorTempo_ = blockTempo_;
    }
    wasPlaying_ = blockPlaying_;
    TransportInfo t;
    t.tempo = blockTempo_;
    t.playing = blockPlaying_;
    t.ppqPosition = ppq_;
    t.numerator = num_.load(std::memory_order_relaxed);
    t.denominator = den_.load(std::memory_order_relaxed);
    return t;
}

void Transport::endBlock(int numSamples, double sampleRate) noexcept {
    if (blockPlaying_ && sampleRate > 0) {
        sinceAnchor_ += numSamples;
        ppq_ = anchorPpq_ + static_cast<double>(sinceAnchor_) * anchorTempo_ / (60.0 * sampleRate);
    }
    ppqShared_.store(ppq_, std::memory_order_relaxed);
}

} // namespace ks
