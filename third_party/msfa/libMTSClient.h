/*
 * keysynth stub for the MTS-ESP client API referenced by Dexed's dx7note.{h,cc}.
 * keysynth does not support MTS-ESP microtuning: there is never a master, so dx7note uses its TuningState.
 *
 * Licensed under the Apache License, Version 2.0 (see LICENSE in this directory).
 */
#ifndef KEYSYNTH_LIBMTSCLIENT_STUB_H
#define KEYSYNTH_LIBMTSCLIENT_STUB_H

#include <math.h>

struct MTSClient;

inline bool MTS_HasMaster(const MTSClient*) { return false; }
inline double MTS_NoteToFrequency(const MTSClient*, char midinote, char /*midichannel*/) {
    return 440.0 * pow(2.0, ((double)midinote - 69.0) / 12.0);
}

#endif
