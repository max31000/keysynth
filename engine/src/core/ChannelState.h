#pragma once
// Per-MIDI-channel controller state maintained by the core (engines don't parse CCs for these, but still
// receive the raw events). The Engine keeps 17 states: index 0 = omni (merged from all channels), 1..16.

#include "core/MidiEvent.h"

namespace ks {

struct ChannelState {
    float pitchBend = 0.0f;  // -1..1
    float modWheel = 0.0f;   // 0..1 (CC1)
    float aftertouch = 0.0f; // 0..1 (channel pressure)
    float expression = 1.0f; // 0..1 (CC11)
    bool sustain = false;    // CC64 >= 64
    bool sostenuto = false;  // CC66
    bool soft = false;       // CC67

    void apply(const MidiEvent& e) noexcept {
        switch (e.type) {
        case MidiEventType::PitchBend: pitchBend = e.valueF; break;
        case MidiEventType::ChannelPressure: aftertouch = e.valueF; break;
        case MidiEventType::ControlChange:
            switch (e.data1) {
            case 1: modWheel = e.valueF; break;
            case 11: expression = e.valueF; break;
            case 64: sustain = e.value7 >= 64; break;
            case 66: sostenuto = e.value7 >= 64; break;
            case 67: soft = e.value7 >= 64; break;
            case 121: *this = ChannelState{}; break; // reset all controllers
            default: break;
            }
            break;
        default: break;
        }
    }
};

} // namespace ks
