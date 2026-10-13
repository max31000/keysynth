#pragma once
// `hold`: hold/latch MIDI processor (ARCHITECTURE §17). While `hold` is on, note-offs are deferred (sustain-like for
// any instrument); Latch mode replaces the held chord on the first key after all keys were up. `pedal`: CC64 drives
// the hold state and is consumed.

#include "core/Module.h"

#include <array>
#include <cstdint>

namespace ks {

class HoldTool final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    HoldTool();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock&, MidiEventSpan, const ProcessContext&) override {}
    void processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext& ctx) override;

    enum P { Hold, Mode, Pedal, Count };
    enum ModeId { Sustain, Latch };

private:
    void releaseHeld(MidiEventBuffer& out, uint32_t offset);

    std::array<bool, 128> down_{};  // key physically down (note-on seen, no note-off yet)
    std::array<bool, 128> held_{};  // sounding with the key up (note-off deferred)
    std::array<uint8_t, 128> channel_{};
    int downCount_ = 0;
    bool active_ = false;   // effective hold state (param or pedal)
    bool pedalDown_ = false;
    bool lastParam_ = false;
};

} // namespace ks
