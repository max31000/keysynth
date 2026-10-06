#pragma once

#include "core/ChannelState.h"

#include <cstdint>

namespace ks {

struct TransportInfo {
    double tempo = 120.0;      // BPM
    double ppqPosition = 0.0;  // quarter notes since start, at block start
    bool playing = false;
    int numerator = 4;
    int denominator = 4;
};

struct ProcessContext {
    double sampleRate = 48000.0;
    int numSamples = 0;
    int64_t sampleTime = 0; // engine sample counter at block start
    TransportInfo transport;
    // Controller state for the module's layer (channel-filtered, sustain masked by the zone). Never null
    // when called by the Engine.
    const ChannelState* channel = nullptr;
};

} // namespace ks
