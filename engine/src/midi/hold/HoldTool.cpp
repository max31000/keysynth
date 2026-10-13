#include "midi/hold/HoldTool.h"

namespace ks {

const ModuleInfo& HoldTool::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "hold";
        i.displayName = "Hold";
        i.kind = ModuleKind::Midi;
        i.category = "MIDI";
        i.params = {
            boolParam("hold", "Hold", true),
            enumParam("mode", "Mode", {"Sustain", "Latch"}, Sustain),
            boolParam("pedal", "Pedal", false),
        };
        i.uiHints = {{"front", {"hold", "mode", "pedal"}}};
        return i;
    }();
    return info;
}

HoldTool::HoldTool() : Module(moduleInfo()) {}

void HoldTool::prepare(double, int) { reset(); }

void HoldTool::reset() {
    down_.fill(false);
    held_.fill(false);
    downCount_ = 0;
    pedalDown_ = false;
    lastParam_ = params().get(Hold) >= 0.5f;
    active_ = params().get(Pedal) >= 0.5f ? false : lastParam_;
}

void HoldTool::releaseHeld(MidiEventBuffer& out, uint32_t offset) {
    for (int n = 0; n < 128; ++n)
        if (held_[static_cast<size_t>(n)]) {
            held_[static_cast<size_t>(n)] = false;
            out.push(MidiEvent::noteOff(n, 0, channel_[static_cast<size_t>(n)], offset));
        }
}

void HoldTool::processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext&) {
    const bool pedal = params().get(Pedal) >= 0.5f;
    const bool param = params().get(Hold) >= 0.5f;
    const bool latch = static_cast<int>(params().get(Mode)) == Latch;
    const bool want = pedal ? pedalDown_ : param;
    if (active_ && !want) releaseHeld(out, 0);
    active_ = want;
    lastParam_ = param;

    for (const MidiEvent& e : in) {
        const size_t n = e.data1;
        switch (e.type) {
        case MidiEventType::NoteOn:
            if (active_ && latch && downCount_ == 0) releaseHeld(out, e.sampleOffset); // new chord replaces
            if (held_[n]) { // re-strike of a held note
                held_[n] = false;
                out.push(MidiEvent::noteOff(e.data1, 0, channel_[n], e.sampleOffset));
            }
            if (!down_[n]) {
                down_[n] = true;
                ++downCount_;
            }
            channel_[n] = e.channel;
            out.push(e);
            break;
        case MidiEventType::NoteOff:
            if (!down_[n]) {
                out.push(e); // not seen: pass on
                break;
            }
            down_[n] = false;
            --downCount_;
            if (active_) held_[n] = true;
            else out.push(e);
            break;
        case MidiEventType::ControlChange:
            if (pedal && e.data1 == 64) {
                pedalDown_ = e.value7 >= 64;
                if (active_ && !pedalDown_) releaseHeld(out, e.sampleOffset);
                active_ = pedalDown_;
                break; // consumed
            }
            out.push(e);
            break;
        case MidiEventType::AllNotesOff:
        case MidiEventType::AllSoundOff:
            releaseHeld(out, e.sampleOffset);
            down_.fill(false);
            downCount_ = 0;
            out.push(e);
            break;
        default: out.push(e); break;
        }
    }
}

} // namespace ks
