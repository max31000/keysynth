#pragma once
// `arp`: arpeggiator MIDI processor (ARCHITECTURE §17). Sample-accurate for any block size: on the transport grid
// while the transport plays and `sync` != Off, otherwise on its own clock in absolute samples. Fixed-size state only.

#include "core/Module.h"
#include "dsp/Noise.h"

#include <array>
#include <cstdint>

namespace ks {

class Arpeggiator final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    Arpeggiator();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock&, MidiEventSpan, const ProcessContext&) override {}
    void processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext& ctx) override;

    enum P { Mode, Octaves, Sync, Rate, Gate, Swing, Latch, Retrigger, VelMode, Velocity, AccentEvery, Count };
    enum ModeId { Up, Down, UpDown, AsPlayed, Random, Chord };
    enum VelModeId { VelAsPlayed, VelFixed, VelAccent };

    static constexpr int kMaxNotes = 16;      // held / latched input notes
    static constexpr int kMaxSeq = kMaxNotes * 4;
    static constexpr int kMaxSounding = 32;

private:
    struct Entry {
        uint8_t note = 0, vel = 0, channel = 1;
        bool down = false;
        uint32_t order = 0; // press order (As played)
    };
    struct Sounding {
        uint8_t note = 0, channel = 1;
        int64_t offAt = 0;  // absolute sample of the note-off
        bool legato = false; // also ends at the next step
    };
    struct Params {
        int mode = 0, octaves = 1, sync = 0, velMode = 0, velocity = 100, accentEvery = 4;
        float rate = 8.0f, gate = 0.5f, swing = 0.0f;
        bool latch = false, retrigger = true;
    };

    // Event helpers (absolute sample time -> block offset).
    void emitOff(MidiEventBuffer& out, int64_t at, uint8_t note, uint8_t channel);
    void releaseAll(MidiEventBuffer& out, int64_t at);
    void noteOn(const MidiEvent& e, int64_t at, MidiEventBuffer& out);
    void noteOff(const MidiEvent& e, MidiEventBuffer& out); // returns via out (pass-through when unknown)
    void startClock(int64_t at, bool resetPattern);
    void stopIfEmpty();
    // Fire pending offs and steps with time < `until` (absolute).
    void advance(int64_t until, MidiEventBuffer& out);
    // Next step time (exact, absolute samples; +inf when none in this block or not running).
    double nextStepExact() const;
    void fireStep(double exact, MidiEventBuffer& out);
    int buildSequence(std::array<Entry, kMaxSeq>& seq) const; // pitch-sorted or press-ordered, all octaves
    int fireSample(double exact) const;                          // block offset of a step / off

    double sr_ = 48000.0;
    Params p_;
    // Block context (valid inside processMidi).
    int64_t t0_ = 0;
    int n_ = 0;
    bool grid_ = false;
    double ppq0_ = 0.0, spp_ = 24000.0; // samples per quarter note
    double stepLen_ = 6000.0;           // samples per step at this block's tempo/rate
    double gridBeats_ = 0.25;

    std::array<Entry, kMaxNotes> chord_{};
    int chordCount_ = 0;
    uint32_t orderCounter_ = 0;
    std::array<Sounding, kMaxSounding> sounding_{};
    int soundingCount_ = 0;

    bool running_ = false;
    int64_t stepCount_ = 0;  // pattern position (steps since the pattern start)
    int lastRandom_ = -1;
    dsp::WhiteNoise rng_{0x51ed2701u};

    // Own clock: step j at anchorTime_ + (j - anchorStep_) * stepLen_ (+ swing on odd j).
    double anchorTime_ = 0.0;
    int64_t anchorStep_ = 0;
    int64_t nextOwn_ = 0;
    double ownLen_ = 0.0; // stepLen_ the anchor was computed with
    // Grid clock: grid step k at ppq k * gridBeats_ (+ swing on odd k).
    int64_t nextGrid_ = 0;
    double gridBeatsUsed_ = 0.0;
    double expectPpq_ = -1.0; // ppq expected at the next block start (detects transport jumps)
    bool lastGrid_ = false;
    double lastStepExact_ = -1.0; // absolute time of the last fired step
};

} // namespace ks
