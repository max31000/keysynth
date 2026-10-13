#include "midi/chord/ChordTool.h"

#include <algorithm>
#include <string>

namespace ks {

namespace {
struct Table {
    int n;
    int iv[5];
};
// Semitones above the root, per TypeId (Memory uses the learned intervals).
constexpr Table kTables[ChordTool::Memory] = {
    {3, {0, 4, 7}},          // Major
    {3, {0, 3, 7}},          // Minor
    {4, {0, 4, 7, 10}},      // 7
    {4, {0, 4, 7, 11}},      // Maj7
    {4, {0, 3, 7, 10}},      // Min7
    {3, {0, 2, 7}},          // Sus2
    {3, {0, 5, 7}},          // Sus4
    {3, {0, 7, 12}},         // Power
    {3, {0, 3, 6}},          // Dim
    {3, {0, 4, 8}},          // Aug
    {4, {0, 4, 7, 14}},      // Add9
    {5, {0, 3, 7, 10, 14}},  // Min9
};
} // namespace

const ModuleInfo& ChordTool::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "chord";
        i.displayName = "Chord";
        i.kind = ModuleKind::Midi;
        i.category = "MIDI";
        i.params = {
            enumParam("type", "Chord",
                      {"Major", "Minor", "7", "Maj7", "Min7", "Sus2", "Sus4", "Power", "Dim", "Aug", "Add9", "Min9", "Memory"},
                      Major, "Chord"),
            enumParam("voicing", "Voicing", {"Close", "Open", "Wide"}, Close, "Chord"),
            intParam("inversion", "Inversion", 0, 3, 0, {}, "Chord"),
        };
        ParamSpec mc = intParam("mem_count", "Memory notes", 0, kMemSlots, 0, {}, "Memory");
        mc.flags = ParamFlags::Hidden;
        i.params.push_back(mc);
        for (int k = 1; k <= kMemSlots; ++k) {
            ParamSpec m = intParam("mem_" + std::to_string(k), "Memory " + std::to_string(k), 0, 48, 0, "st", "Memory");
            m.flags = ParamFlags::Hidden;
            i.params.push_back(m);
        }
        ParamSpec hc = intParam("held_count", "Held notes", 0, 16, 0, {}, "Input");
        hc.flags = ParamFlags::ReadOnly | ParamFlags::Hidden;
        i.params.push_back(hc);
        for (int k = 1; k <= kHeldReadouts; ++k) {
            ParamSpec h = intParam("held_" + std::to_string(k), "Held " + std::to_string(k), -1, 127, -1, {}, "Input");
            h.flags = ParamFlags::ReadOnly | ParamFlags::Hidden;
            i.params.push_back(h);
        }
        i.uiHints = {{"front", {"type", "voicing", "inversion"}}};
        return i;
    }();
    return info;
}

ChordTool::ChordTool() : Module(moduleInfo()) {}

void ChordTool::prepare(double, int) { reset(); }

void ChordTool::reset() {
    mapCount_.fill(0);
    mapped_.fill(false);
    ref_.fill(0);
    heldCount_ = 0;
    params().setRaw(HeldCount, 0.0f);
    for (int k = 0; k < kHeldReadouts; ++k) params().setRaw(Held1 + k, -1.0f);
}

int ChordTool::chordTones(int root, int type, int voicing, int inversion, const int* mem, int memCount,
                          std::array<int, kMaxTones>& out) noexcept {
    std::array<int, kMaxTones> t{};
    int n = 0;
    if (type >= 0 && type < Memory) {
        const Table& tb = kTables[type];
        for (int i = 0; i < tb.n; ++i) t[static_cast<size_t>(n++)] = tb.iv[i];
    } else {
        t[static_cast<size_t>(n++)] = 0;
        for (int i = 0; i < memCount && i < kMemSlots && n < kMaxTones - 1; ++i)
            if (mem[i] > 0) t[static_cast<size_t>(n++)] = mem[i];
    }
    std::sort(t.begin(), t.begin() + n);
    // Inversion: the lowest k tones up an octave.
    const int inv = std::clamp(inversion, 0, std::max(0, n - 1));
    for (int i = 0; i < inv; ++i) t[static_cast<size_t>(i)] += 12;
    std::sort(t.begin(), t.begin() + n);
    if (voicing == Open || voicing == Wide)
        for (int i = 1; i < n; i += 2) t[static_cast<size_t>(i)] += 12;
    if (voicing == Wide && n < kMaxTones) t[static_cast<size_t>(n++)] = t[0] - 12;
    std::sort(t.begin(), t.begin() + n);
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const int note = root + t[static_cast<size_t>(i)];
        if (note < 0 || note > 127) continue;
        if (m > 0 && out[static_cast<size_t>(m - 1)] == note) continue;
        out[static_cast<size_t>(m++)] = note;
    }
    return m;
}

void ChordTool::processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext&) {
    const ParamSet& ps = params();
    const int type = static_cast<int>(ps.get(Type));
    const int voicing = static_cast<int>(ps.get(Voicing));
    const int inversion = static_cast<int>(ps.get(Inversion));
    const int memCount = static_cast<int>(ps.get(MemCount));
    int mem[kMemSlots];
    for (int k = 0; k < kMemSlots; ++k) mem[k] = static_cast<int>(ps.get(Mem1 + k));

    auto release = [&](int note, uint32_t off) {
        if (!mapped_[static_cast<size_t>(note)]) return false;
        for (int i = 0; i < mapCount_[static_cast<size_t>(note)]; ++i) {
            const uint8_t t = map_[static_cast<size_t>(note)][static_cast<size_t>(i)];
            if (ref_[t] > 0 && --ref_[t] == 0) out.push(MidiEvent::noteOff(t, 0, toneChannel_[t], off));
        }
        mapped_[static_cast<size_t>(note)] = false;
        mapCount_[static_cast<size_t>(note)] = 0;
        return true;
    };
    auto heldRemove = [&](int note) {
        for (int i = 0; i < heldCount_; ++i)
            if (held_[static_cast<size_t>(i)] == note) {
                for (int j = i; j + 1 < heldCount_; ++j) held_[static_cast<size_t>(j)] = held_[static_cast<size_t>(j + 1)];
                --heldCount_;
                return;
            }
    };

    for (const MidiEvent& e : in) {
        switch (e.type) {
        case MidiEventType::NoteOn: {
            const int root = e.data1;
            release(root, e.sampleOffset); // re-press without a note-off
            heldRemove(root);
            if (heldCount_ < static_cast<int>(held_.size())) {
                int i = heldCount_++;
                while (i > 0 && held_[static_cast<size_t>(i - 1)] > root) {
                    held_[static_cast<size_t>(i)] = held_[static_cast<size_t>(i - 1)];
                    --i;
                }
                held_[static_cast<size_t>(i)] = static_cast<uint8_t>(root);
            }
            std::array<int, kMaxTones> tones{};
            const int n = chordTones(root, type, voicing, inversion, mem, memCount, tones);
            int c = 0;
            for (int i = 0; i < n; ++i) {
                const auto t = static_cast<uint8_t>(tones[static_cast<size_t>(i)]);
                if (ref_[t] == 0) {
                    toneChannel_[t] = e.channel;
                    out.push(MidiEvent::noteOn(t, e.value7, e.channel, e.sampleOffset));
                }
                if (ref_[t] < 255) ++ref_[t];
                map_[static_cast<size_t>(root)][static_cast<size_t>(c++)] = t;
            }
            mapCount_[static_cast<size_t>(root)] = static_cast<uint8_t>(c);
            mapped_[static_cast<size_t>(root)] = true;
            break;
        }
        case MidiEventType::NoteOff:
            heldRemove(e.data1);
            if (!release(e.data1, e.sampleOffset)) out.push(e); // not ours: pass on
            break;
        case MidiEventType::PolyPressure:
            if (mapped_[e.data1]) {
                for (int i = 0; i < mapCount_[e.data1]; ++i) {
                    MidiEvent x = e;
                    x.data1 = map_[e.data1][static_cast<size_t>(i)];
                    out.push(x);
                }
            } else {
                out.push(e);
            }
            break;
        case MidiEventType::AllNotesOff:
        case MidiEventType::AllSoundOff:
            for (int t = 0; t < 128; ++t)
                if (ref_[static_cast<size_t>(t)] > 0) out.push(MidiEvent::noteOff(t, 0, toneChannel_[static_cast<size_t>(t)], e.sampleOffset));
            ref_.fill(0);
            mapped_.fill(false);
            mapCount_.fill(0);
            heldCount_ = 0;
            out.push(e);
            break;
        default: out.push(e); break;
        }
    }
    ParamSet& w = params();
    w.setRaw(HeldCount, static_cast<float>(heldCount_));
    for (int k = 0; k < kHeldReadouts; ++k)
        w.setRaw(Held1 + k, k < heldCount_ ? static_cast<float>(held_[static_cast<size_t>(k)]) : -1.0f);
}

} // namespace ks
