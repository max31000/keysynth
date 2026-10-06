#include "core/MidiEvent.h"

#include <algorithm>

namespace ks {

namespace {
uint8_t u7(int v) noexcept { return static_cast<uint8_t>(std::clamp(v, 0, 127)); }
uint8_t ch(int c) noexcept { return static_cast<uint8_t>(std::clamp(c, 1, 16)); }
} // namespace

MidiEvent MidiEvent::noteOn(int note, int velocity, int channel, uint32_t offset) noexcept {
    MidiEvent e;
    e.sampleOffset = offset;
    e.type = velocity > 0 ? MidiEventType::NoteOn : MidiEventType::NoteOff;
    e.channel = ch(channel);
    e.data1 = u7(note);
    e.value7 = u7(velocity);
    e.valueF = static_cast<float>(e.value7) / 127.0f;
    return e;
}

MidiEvent MidiEvent::noteOff(int note, int velocity, int channel, uint32_t offset) noexcept {
    MidiEvent e;
    e.sampleOffset = offset;
    e.type = MidiEventType::NoteOff;
    e.channel = ch(channel);
    e.data1 = u7(note);
    e.value7 = u7(velocity);
    e.valueF = static_cast<float>(e.value7) / 127.0f;
    return e;
}

MidiEvent MidiEvent::cc(int number, int value, int channel, uint32_t offset) noexcept {
    MidiEvent e;
    e.sampleOffset = offset;
    e.type = MidiEventType::ControlChange;
    e.channel = ch(channel);
    e.data1 = u7(number);
    e.value7 = u7(value);
    e.valueF = static_cast<float>(e.value7) / 127.0f;
    return e;
}

MidiEvent MidiEvent::pitchBend(float value, int channel, uint32_t offset) noexcept {
    MidiEvent e;
    e.sampleOffset = offset;
    e.type = MidiEventType::PitchBend;
    e.channel = ch(channel);
    e.valueF = std::clamp(value, -1.0f, 1.0f);
    return e;
}

MidiEvent MidiEvent::allNotesOff(uint32_t offset) noexcept {
    MidiEvent e;
    e.sampleOffset = offset;
    e.type = MidiEventType::AllNotesOff;
    e.channel = 0;
    return e;
}

MidiEvent MidiEvent::allSoundOff(uint32_t offset) noexcept {
    MidiEvent e;
    e.sampleOffset = offset;
    e.type = MidiEventType::AllSoundOff;
    e.channel = 0;
    return e;
}

bool MidiEvent::fromBytes(const uint8_t* d, int size, MidiEvent& out) noexcept {
    if (d == nullptr || size < 1) return false;
    const uint8_t status = d[0];
    if (status < 0x80 || status >= 0xF0) return false;
    const int channel = (status & 0x0F) + 1;
    const int hi = status & 0xF0;
    const int b1 = size > 1 ? (d[1] & 0x7F) : 0;
    const int b2 = size > 2 ? (d[2] & 0x7F) : 0;
    switch (hi) {
    case 0x80: out = noteOff(b1, b2, channel); return size >= 3;
    case 0x90: out = noteOn(b1, b2, channel); return size >= 3;
    case 0xA0:
        out = MidiEvent{};
        out.type = MidiEventType::PolyPressure;
        out.channel = ch(channel);
        out.data1 = u7(b1);
        out.value7 = u7(b2);
        out.valueF = static_cast<float>(b2) / 127.0f;
        return size >= 3;
    case 0xB0:
        if (size < 3) return false;
        if (b1 == 123) { out = allNotesOff(); out.channel = ch(channel); return true; }
        if (b1 == 120) { out = allSoundOff(); out.channel = ch(channel); return true; }
        out = cc(b1, b2, channel);
        return true;
    case 0xC0:
        out = MidiEvent{};
        out.type = MidiEventType::ProgramChange;
        out.channel = ch(channel);
        out.data1 = u7(b1);
        return size >= 2;
    case 0xD0:
        out = MidiEvent{};
        out.type = MidiEventType::ChannelPressure;
        out.channel = ch(channel);
        out.value7 = u7(b1);
        out.valueF = static_cast<float>(b1) / 127.0f;
        return size >= 2;
    case 0xE0: {
        if (size < 3) return false;
        const int v = (b2 << 7) | b1; // 0..16383, centre 8192
        const float f = v >= 8192 ? static_cast<float>(v - 8192) / 8191.0f : static_cast<float>(v - 8192) / 8192.0f;
        out = pitchBend(f, channel);
        return true;
    }
    default: return false;
    }
}

} // namespace ks
