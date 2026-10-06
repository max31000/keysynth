#pragma once
// Fixed-size voice allocator (ARCHITECTURE §5.2): poly / mono / legato, stealing (released first, then oldest),
// sustain (CC64) and sostenuto (CC66), glide hook. Unison stays inside engines. RT-safe: no allocation.
//
// VoiceT requirements:
//   void noteOn(const VoiceStart& s);  // start or (legato) retarget the voice
//   void noteOff();                    // enter release
//   void kill();                       // fast fade (used when stealing / AllSoundOff may use reset)
//   void reset();                      // immediate silence
//   bool isActive() const;             // still producing sound (incl. release tail)

#include "core/MidiEvent.h"

#include <array>
#include <cstdint>

namespace ks {

enum class VoiceMode { Poly = 0, Mono = 1, Legato = 2 };

struct VoiceStart {
    int note = 60;            // MIDI note (after zone transpose)
    float velocity = 1.0f;    // 0..1
    int32_t noteId = -1;
    int glideFrom = -1;       // previous note for portamento (-1 = none); voice decides glide time
    bool legato = false;      // true: retarget pitch without retriggering envelopes
    bool stolen = false;      // the voice was sounding something else (engine may crossfade)
};

template <typename VoiceT, int N>
class VoiceAllocator {
    static_assert(N > 0 && N <= 128);

public:
    void setMode(VoiceMode m) noexcept {
        if (m == mode_) return;
        allNotesOff();
        mode_ = m;
        stackSize_ = 0;
    }
    VoiceMode mode() const noexcept { return mode_; }

    // Number of voices used in Poly mode (1..N).
    void setPolyphony(int n) noexcept { polyphony_ = n < 1 ? 1 : (n > N ? N : n); }
    int polyphony() const noexcept { return polyphony_; }

    // When false, sustain pedal events are ignored (zone "sustain": false is applied upstream too).
    void setSustainEnabled(bool on) noexcept { sustainEnabled_ = on; }

    VoiceT& voice(int i) noexcept { return voices_[static_cast<size_t>(i)]; }
    const VoiceT& voice(int i) const noexcept { return voices_[static_cast<size_t>(i)]; }
    static constexpr int maxVoices() noexcept { return N; }

    template <typename F>
    void forEachActive(F&& f) noexcept {
        for (int i = 0; i < N; ++i)
            if (voices_[static_cast<size_t>(i)].isActive()) f(voices_[static_cast<size_t>(i)]);
    }

    int activeCount() const noexcept {
        int c = 0;
        for (const auto& v : voices_) c += v.isActive() ? 1 : 0;
        return c;
    }

    // Feed every event in sample order (the module renders up to e.sampleOffset first).
    void handleEvent(const MidiEvent& e) noexcept {
        switch (e.type) {
        case MidiEventType::NoteOn: noteOn(e.data1, e.valueF, e.noteId); break;
        case MidiEventType::NoteOff: noteOff(e.data1); break;
        case MidiEventType::ControlChange:
            if (e.data1 == 64) setSustain(e.value7 >= 64);
            else if (e.data1 == 66) setSostenuto(e.value7 >= 64);
            break;
        case MidiEventType::AllNotesOff: allNotesOff(); break;
        case MidiEventType::AllSoundOff: reset(); break;
        default: break;
        }
    }

    void noteOn(int note, float velocity, int32_t noteId = -1) noexcept {
        if (note < 0 || note > 127) return;
        ++clock_;
        if (mode_ == VoiceMode::Poly) polyNoteOn(note, velocity, noteId);
        else monoNoteOn(note, velocity, noteId);
    }

    void noteOff(int note) noexcept {
        if (note < 0 || note > 127) return;
        if (mode_ == VoiceMode::Poly) {
            for (int i = 0; i < N; ++i) {
                auto& s = slots_[static_cast<size_t>(i)];
                if (s.held && s.note == note) releaseOrSustain(i);
            }
        } else {
            monoNoteOff(note);
        }
    }

    void setSustain(bool down) noexcept {
        if (!sustainEnabled_) down = false;
        sustain_ = down;
        if (!down) {
            for (int i = 0; i < N; ++i) {
                auto& s = slots_[static_cast<size_t>(i)];
                if (s.sustained && !s.held && !s.sostenuto) {
                    s.sustained = false;
                    voices_[static_cast<size_t>(i)].noteOff();
                } else if (!s.held) {
                    s.sustained = false;
                }
            }
        }
    }

    void setSostenuto(bool down) noexcept {
        if (down && !sostenuto_) {
            for (auto& s : slots_)
                if (s.held) s.sostenuto = true;
        } else if (!down && sostenuto_) {
            for (int i = 0; i < N; ++i) {
                auto& s = slots_[static_cast<size_t>(i)];
                if (!s.sostenuto) continue;
                s.sostenuto = false;
                if (!s.held && !(sustain_ && s.sustained)) {
                    s.sustained = false;
                    voices_[static_cast<size_t>(i)].noteOff();
                }
            }
        }
        sostenuto_ = down;
    }

    // Release everything regardless of pedals (pedal state itself is kept: the pedal may still be down).
    void allNotesOff() noexcept {
        for (int i = 0; i < N; ++i) {
            auto& s = slots_[static_cast<size_t>(i)];
            if (s.held || s.sustained || s.sostenuto) voices_[static_cast<size_t>(i)].noteOff();
            s.held = s.sustained = s.sostenuto = false;
        }
        stackSize_ = 0;
    }

    // Immediate silence, forget all state.
    void reset() noexcept {
        for (int i = 0; i < N; ++i) {
            voices_[static_cast<size_t>(i)].reset();
            slots_[static_cast<size_t>(i)] = Slot{};
        }
        stackSize_ = 0;
        sustain_ = sostenuto_ = false;
        lastNote_ = -1;
    }

    // Introspection (tests / engines).
    bool isHeld(int voiceIndex) const noexcept { return slots_[static_cast<size_t>(voiceIndex)].held; }
    bool isSustained(int voiceIndex) const noexcept { return slots_[static_cast<size_t>(voiceIndex)].sustained; }
    int noteOf(int voiceIndex) const noexcept { return slots_[static_cast<size_t>(voiceIndex)].note; }

private:
    struct Slot {
        int note = -1;
        int32_t noteId = -1;
        uint64_t age = 0; // clock at start
        bool held = false;      // key physically down
        bool sustained = false; // key released while sustain pedal down
        bool sostenuto = false; // latched by sostenuto pedal
    };

    void releaseOrSustain(int i) noexcept {
        auto& s = slots_[static_cast<size_t>(i)];
        s.held = false;
        if (sustain_ || s.sostenuto) {
            s.sustained = true;
            return;
        }
        voices_[static_cast<size_t>(i)].noteOff();
    }

    int findVoiceForNote(int note) const noexcept {
        for (int i = 0; i < N; ++i) { // all voices: polyphony may have been lowered while notes sound
            const auto& s = slots_[static_cast<size_t>(i)];
            if (s.note == note && voices_[static_cast<size_t>(i)].isActive()) return i;
        }
        return -1;
    }

    int pickVoice() const noexcept {
        // 1) free voice
        for (int i = 0; i < polyphony_; ++i)
            if (!voices_[static_cast<size_t>(i)].isActive()) return i;
        // 2) oldest released (not held, not sustained)
        int best = -1;
        uint64_t bestAge = ~uint64_t{0};
        for (int i = 0; i < polyphony_; ++i) {
            const auto& s = slots_[static_cast<size_t>(i)];
            if (!s.held && !s.sustained && !s.sostenuto && s.age < bestAge) {
                best = i;
                bestAge = s.age;
            }
        }
        if (best >= 0) return best;
        // 3) oldest sustained-but-not-held
        for (int i = 0; i < polyphony_; ++i) {
            const auto& s = slots_[static_cast<size_t>(i)];
            if (!s.held && s.age < bestAge) {
                best = i;
                bestAge = s.age;
            }
        }
        if (best >= 0) return best;
        // 4) oldest overall
        for (int i = 0; i < polyphony_; ++i) {
            const auto& s = slots_[static_cast<size_t>(i)];
            if (s.age < bestAge) {
                best = i;
                bestAge = s.age;
            }
        }
        return best < 0 ? 0 : best;
    }

    void polyNoteOn(int note, float velocity, int32_t noteId) noexcept {
        // Re-striking a sounding note reuses its voice (no stacking of identical notes).
        int i = findVoiceForNote(note);
        const bool reuse = i >= 0;
        if (!reuse) i = pickVoice();
        auto& s = slots_[static_cast<size_t>(i)];
        auto& v = voices_[static_cast<size_t>(i)];
        VoiceStart st;
        st.note = note;
        st.velocity = velocity;
        st.noteId = noteId;
        st.glideFrom = lastNote_;
        st.stolen = !reuse && v.isActive();
        if (st.stolen) v.kill();
        const bool keepSostenuto = reuse && s.sostenuto;
        s = Slot{note, noteId, clock_, true, false, keepSostenuto};
        v.noteOn(st);
        lastNote_ = note;
    }

    void stackRemove(int note) noexcept {
        int w = 0;
        for (int r = 0; r < stackSize_; ++r)
            if (stack_[static_cast<size_t>(r)] != note) stack_[static_cast<size_t>(w++)] = stack_[static_cast<size_t>(r)];
        stackSize_ = w;
    }

    void monoNoteOn(int note, float velocity, int32_t noteId) noexcept {
        const bool wasHeld = stackSize_ > 0;
        stackRemove(note);
        if (stackSize_ == static_cast<int>(stack_.size())) {
            for (int r = 1; r < stackSize_; ++r) stack_[static_cast<size_t>(r - 1)] = stack_[static_cast<size_t>(r)];
            --stackSize_;
        }
        stack_[static_cast<size_t>(stackSize_++)] = note;
        lastVelocity_ = velocity;
        auto& s = slots_[0];
        auto& v = voices_[0];
        VoiceStart st;
        st.note = note;
        st.velocity = velocity;
        st.noteId = noteId;
        st.glideFrom = v.isActive() ? s.note : lastNote_;
        st.legato = mode_ == VoiceMode::Legato && wasHeld && v.isActive();
        s = Slot{note, noteId, clock_, true, false, false};
        v.noteOn(st);
        lastNote_ = note;
    }

    void monoNoteOff(int note) noexcept {
        auto& s = slots_[0];
        const bool wasCurrent = s.held && s.note == note;
        stackRemove(note);
        if (!wasCurrent) return;
        if (stackSize_ > 0) {
            // Return to the previous held note (legato transition, glide from current).
            const int prev = stack_[static_cast<size_t>(stackSize_ - 1)];
            VoiceStart st;
            st.note = prev;
            st.velocity = lastVelocity_;
            st.glideFrom = s.note;
            st.legato = mode_ == VoiceMode::Legato; // Mono retriggers, Legato glides
            s.note = prev;
            voices_[0].noteOn(st);
            lastNote_ = prev;
            return;
        }
        releaseOrSustain(0);
    }

    std::array<VoiceT, N> voices_{};
    std::array<Slot, N> slots_{};
    std::array<int, 16> stack_{};
    int stackSize_ = 0;
    int polyphony_ = N;
    VoiceMode mode_ = VoiceMode::Poly;
    bool sustain_ = false;
    bool sostenuto_ = false;
    bool sustainEnabled_ = true;
    int lastNote_ = -1;
    float lastVelocity_ = 1.0f;
    uint64_t clock_ = 0;
};

} // namespace ks
