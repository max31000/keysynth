/*
 * keysynth replacement for Dexed's msfa/tuning.h.
 *
 * The Dexed version depends on the Surge "Tunings" library and JUCE (SCL/KBM microtuning). keysynth only needs
 * 12-TET, so this file keeps the TuningState interface used by dx7note.cc and provides the standard tuning only.
 * The logfreq formula is unchanged from Dexed/MSFA.
 *
 * Licensed under the Apache License, Version 2.0 (see LICENSE in this directory).
 */
#ifndef __SYNTH_TUNING_H
#define __SYNTH_TUNING_H

#include "synth.h"
#include <memory>

class TuningState {
public:
    virtual ~TuningState() { }

    virtual int32_t midinote_to_logfreq(int midinote) = 0;
    virtual bool is_standard_tuning() { return true; }
    virtual int scale_length() { return 12; }
};

std::shared_ptr<TuningState> createStandardTuning();

#endif
