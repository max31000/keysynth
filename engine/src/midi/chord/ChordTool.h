#pragma once
// `chord`: one-finger chords / chord memory MIDI processor (ARCHITECTURE §17). Each input note plays a chord from a
// fixed table or the learned memory (hidden params mem_*); output tones are reference-counted. ReadOnly readouts
// held_count / held_1..held_8 expose the current input notes for `chord_learn` (Session::learnChord).

#include "core/Module.h"

#include <array>
#include <cstdint>

namespace ks {

class ChordTool final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    ChordTool();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock&, MidiEventSpan, const ProcessContext&) override {}
    void processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext& ctx) override;

    static constexpr int kMemSlots = 8;
    static constexpr int kHeldReadouts = 8;
    static constexpr int kMaxTones = 10;
    enum P {
        Type, Voicing, Inversion, MemCount,
        Mem1, // .. Mem1 + kMemSlots - 1
        HeldCount = Mem1 + kMemSlots,
        Held1, // .. Held1 + kHeldReadouts - 1
        Count = Held1 + kHeldReadouts
    };
    enum TypeId { Major, Minor, Dom7, Maj7, Min7, Sus2, Sus4, Power, Dim, Aug, Add9, Min9, Memory, NumTypes };
    enum VoicingId { Close, Open, Wide };

    // Chord tones for `root` with the given settings (sorted, deduplicated, inside 0..127). Pure, RT-safe.
    static int chordTones(int root, int type, int voicing, int inversion, const int* mem, int memCount,
                          std::array<int, kMaxTones>& out) noexcept;

private:
    std::array<std::array<uint8_t, kMaxTones>, 128> map_{}; // input note -> output tones
    std::array<uint8_t, 128> mapCount_{};
    std::array<bool, 128> mapped_{};
    std::array<uint8_t, 128> ref_{}; // output tone refcount
    std::array<uint8_t, 128> toneChannel_{};
    std::array<uint8_t, 16> held_{}; // input notes down, sorted
    int heldCount_ = 0;
};

} // namespace ks
