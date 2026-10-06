/*
 * keysynth replacement for Dexed's msfa/tuning.cc: 12-TET only (no SCL/KBM, no JUCE dependency).
 * The table formula is unchanged from Dexed/MSFA.
 *
 * Licensed under the Apache License, Version 2.0 (see LICENSE in this directory).
 */
#include "tuning.h"

namespace {
struct StandardTuning : public TuningState {
    StandardTuning() {
        const int base = 50857777;  // (1 << 24) * (log(440) / log(2) - 69/12)
        const int step = (1 << 24) / 12;
        for (int mn = 0; mn < 128; ++mn) table_[mn] = base + step * mn;
    }
    int32_t midinote_to_logfreq(int midinote) override {
        if (midinote < 0) midinote = 0;
        if (midinote > 127) midinote = 127;
        return table_[midinote];
    }
    int32_t table_[128];
};
}  // namespace

std::shared_ptr<TuningState> createStandardTuning() { return std::make_shared<StandardTuning>(); }
