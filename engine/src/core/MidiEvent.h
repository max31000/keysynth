#pragma once
// POD MIDI event passed to modules (ARCHITECTURE §5.2). channel is 1..16 (0 = internal/all).

#include <cstddef>
#include <cstdint>
#include <span>

namespace ks {

enum class MidiEventType : uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    PitchBend,       // valueF in -1..1
    ChannelPressure, // valueF in 0..1
    PolyPressure,    // note + valueF
    ProgramChange,
    AllNotesOff,     // release everything (ignores sustain); sent by the core on transitions/panic
    AllSoundOff,     // hard stop (reset tails)
};

struct MidiEvent {
    uint32_t sampleOffset = 0; // offset inside the current block
    MidiEventType type = MidiEventType::NoteOn;
    uint8_t channel = 1;  // 1..16
    uint8_t data1 = 0;    // note or cc number
    uint8_t value7 = 0;   // 7-bit value (velocity, cc value)
    float valueF = 0.0f;  // normalized value (velocity/127, cc/127, bend -1..1)
    int32_t noteId = -1;  // MPE/MIDI2-ready voice id; -1 = derive from note

    static MidiEvent noteOn(int note, int velocity, int channel = 1, uint32_t offset = 0) noexcept;
    static MidiEvent noteOff(int note, int velocity = 0, int channel = 1, uint32_t offset = 0) noexcept;
    static MidiEvent cc(int number, int value, int channel = 1, uint32_t offset = 0) noexcept;
    static MidiEvent pitchBend(float value, int channel = 1, uint32_t offset = 0) noexcept;
    static MidiEvent allNotesOff(uint32_t offset = 0) noexcept;
    static MidiEvent allSoundOff(uint32_t offset = 0) noexcept;

    // Parses a short MIDI message. Returns false for messages we ignore (sysex, clock, ...).
    // NoteOn with velocity 0 becomes NoteOff. CC 123 → AllNotesOff, CC 120 → AllSoundOff.
    static bool fromBytes(const uint8_t* data, int size, MidiEvent& out) noexcept;

    bool isNote() const noexcept { return type == MidiEventType::NoteOn || type == MidiEventType::NoteOff; }
};

using MidiEventSpan = std::span<const MidiEvent>;

// Output of a MIDI processor (ARCHITECTURE §17): a fixed-capacity, non-owning view over preallocated storage.
// RT-safe; push() drops the event when full.
class MidiEventBuffer {
public:
    MidiEventBuffer(MidiEvent* data, size_t capacity) noexcept : data_(data), capacity_(capacity) {}
    bool push(const MidiEvent& e) noexcept {
        if (size_ >= capacity_) return false;
        data_[size_++] = e;
        return true;
    }
    void clear() noexcept { size_ = 0; }
    void truncate(size_t n) noexcept {
        if (n < size_) size_ = n;
    }
    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return capacity_; }
    bool full() const noexcept { return size_ >= capacity_; }
    MidiEvent* data() noexcept { return data_; }
    MidiEventSpan span() const noexcept { return MidiEventSpan(data_, size_); }
    // Stable insertion sort by sampleOffset (no allocation; event counts per block are small).
    void sortByOffset() noexcept {
        for (size_t i = 1; i < size_; ++i) {
            const MidiEvent e = data_[i];
            size_t j = i;
            while (j > 0 && data_[j - 1].sampleOffset > e.sampleOffset) {
                data_[j] = data_[j - 1];
                --j;
            }
            data_[j] = e;
        }
    }

private:
    MidiEvent* data_;
    size_t capacity_;
    size_t size_ = 0;
};

} // namespace ks
