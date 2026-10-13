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

// Device input bus of the current block (ARCHITECTURE §6 "Audio input"). Read-only view, ProcessContext::numSamples
// valid samples. channels 0 = no input (null pointers), 1 = mono (right == left), 2 = stereo.
struct ExternalInput {
    const float* left = nullptr;
    const float* right = nullptr;
    int channels = 0;

    bool present() const noexcept { return channels > 0 && left != nullptr && right != nullptr; }
    ExternalInput offset(int samples) const noexcept {
        return present() ? ExternalInput{left + samples, right + samples, channels} : ExternalInput{};
    }
};

struct ProcessContext {
    double sampleRate = 48000.0;
    int numSamples = 0;
    int64_t sampleTime = 0; // engine sample counter at block start
    TransportInfo transport;
    // Controller state for the module's layer (channel-filtered, sustain masked by the zone). Never null
    // when called by the Engine.
    const ChannelState* channel = nullptr;
    ExternalInput input; // device input bus (no input by default)
};

} // namespace ks
