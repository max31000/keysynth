#include "midi/arp/Arpeggiator.h"

#include "dsp/NoteDivision.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ks {

namespace {
constexpr double kEps = 1e-4; // samples (same rule as the DrumSequencer)
constexpr double kNever = std::numeric_limits<double>::infinity();
constexpr int kMaxEventsPerAdvance = 1024; // guard against runaway loops
} // namespace

const ModuleInfo& Arpeggiator::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "arp";
        i.displayName = "Arpeggiator";
        i.kind = ModuleKind::Midi;
        i.category = "MIDI";
        i.params = {
            enumParam("mode", "Mode", {"Up", "Down", "Up-Down", "As played", "Random", "Chord"}, Up, "Pattern"),
            intParam("octaves", "Octaves", 1, 4, 1, {}, "Pattern"),
            enumParam("sync", "Sync", dsp::noteDivisionChoices(), 13, "Rate"), // 1/16
            logParam("rate", "Rate", 0.5f, 50.0f, 8.0f, "Hz", "Rate"),
            linearParam("gate", "Gate", 0.05f, 1.0f, 0.5f, {}, "Rate"),
            linearParam("swing", "Swing", 0.0f, 1.0f, 0.0f, {}, "Rate"),
            boolParam("latch", "Latch", false, "Pattern"),
            boolParam("retrigger", "Retrigger", true, "Pattern"),
            enumParam("vel_mode", "Velocity", {"As played", "Fixed", "Accent"}, VelAsPlayed, "Velocity"),
            intParam("velocity", "Level", 1, 127, 100, {}, "Velocity"),
            intParam("accent_every", "Accent every", 2, 16, 4, "steps", "Velocity"),
        };
        i.uiHints = {{"groupOrder", {"Pattern", "Rate", "Velocity"}},
                     {"front", {"mode", "octaves", "sync", "gate", "latch"}}};
        return i;
    }();
    return info;
}

Arpeggiator::Arpeggiator() : Module(moduleInfo()) {}

void Arpeggiator::prepare(double sampleRate, int) {
    sr_ = sampleRate > 0 ? sampleRate : 48000.0;
    reset();
}

void Arpeggiator::reset() {
    chordCount_ = 0;
    soundingCount_ = 0;
    running_ = false;
    stepCount_ = 0;
    lastRandom_ = -1;
    rng_.setSeed(0x51ed2701u);
    anchorTime_ = 0.0;
    anchorStep_ = nextOwn_ = 0;
    ownLen_ = 0.0;
    nextGrid_ = 0;
    gridBeatsUsed_ = 0.0;
    expectPpq_ = -1.0;
    lastGrid_ = false;
    lastStepExact_ = -1.0;
    orderCounter_ = 0;
}

int Arpeggiator::fireSample(double exact) const {
    const double rel = exact - static_cast<double>(t0_);
    if (rel <= kEps) return 0;
    return static_cast<int>(std::ceil(rel - kEps));
}

void Arpeggiator::emitOff(MidiEventBuffer& out, int64_t at, uint8_t note, uint8_t channel) {
    const int64_t rel = at - t0_;
    const uint32_t off = static_cast<uint32_t>(std::clamp<int64_t>(rel, 0, std::max(0, n_ - 1)));
    out.push(MidiEvent::noteOff(note, 0, channel, off));
}

void Arpeggiator::releaseAll(MidiEventBuffer& out, int64_t at) {
    for (int i = 0; i < soundingCount_; ++i) emitOff(out, at, sounding_[static_cast<size_t>(i)].note, sounding_[static_cast<size_t>(i)].channel);
    soundingCount_ = 0;
    chordCount_ = 0;
    running_ = false;
}

double Arpeggiator::nextStepExact() const {
    if (!running_) return kNever;
    if (grid_) {
        const double sw = (nextGrid_ & 1) ? static_cast<double>(p_.swing) * 0.5 * gridBeats_ : 0.0;
        const double ppq = static_cast<double>(nextGrid_) * gridBeats_ + sw;
        return static_cast<double>(t0_) + (ppq - ppq0_) * spp_;
    }
    const double sw = (nextOwn_ & 1) ? static_cast<double>(p_.swing) * 0.5 * ownLen_ : 0.0;
    return anchorTime_ + static_cast<double>(nextOwn_ - anchorStep_) * ownLen_ + sw;
}

void Arpeggiator::startClock(int64_t at, bool resetPattern) {
    if (resetPattern) {
        stepCount_ = 0;
        lastRandom_ = -1;
    }
    running_ = true;
    lastStepExact_ = -1.0;
    if (grid_) {
        // First grid point at or after `at` (an odd, swung point before the next even one may still qualify).
        const double ppqAt = ppq0_ + static_cast<double>(at - t0_) / spp_;
        const double tol = kEps / spp_;
        int64_t k = static_cast<int64_t>(std::ceil((ppqAt - tol) / gridBeats_));
        const int64_t prev = k - 1;
        if ((prev & 1) && p_.swing > 0.0f) {
            const double ppqPrev = static_cast<double>(prev) * gridBeats_ + static_cast<double>(p_.swing) * 0.5 * gridBeats_;
            if (ppqPrev >= ppqAt - tol) k = prev;
        }
        nextGrid_ = k;
    } else {
        anchorTime_ = static_cast<double>(at);
        anchorStep_ = 0;
        nextOwn_ = 0;
        ownLen_ = stepLen_;
    }
}

void Arpeggiator::stopIfEmpty() {
    if (chordCount_ == 0) running_ = false;
}

void Arpeggiator::noteOn(const MidiEvent& e, int64_t at, MidiEventBuffer&) {
    bool anyDown = false;
    for (int i = 0; i < chordCount_; ++i) anyDown = anyDown || chord_[static_cast<size_t>(i)].down;
    const bool replace = p_.latch && !anyDown && chordCount_ > 0;
    const bool newChord = chordCount_ == 0 || replace;
    if (replace) chordCount_ = 0;
    int idx = -1;
    for (int i = 0; i < chordCount_; ++i)
        if (chord_[static_cast<size_t>(i)].note == e.data1) idx = i;
    if (idx < 0) {
        if (chordCount_ >= kMaxNotes) return; // full: ignored
        idx = chordCount_++;
        chord_[static_cast<size_t>(idx)].order = orderCounter_++;
    }
    Entry& en = chord_[static_cast<size_t>(idx)];
    en.note = e.data1;
    en.vel = e.value7;
    en.channel = e.channel;
    en.down = true;
    if (newChord) {
        if (!running_) startClock(at, p_.retrigger);
        else if (p_.retrigger) startClock(at, true);
    }
}

void Arpeggiator::noteOff(const MidiEvent& e, MidiEventBuffer& out) {
    int idx = -1;
    for (int i = 0; i < chordCount_; ++i)
        if (chord_[static_cast<size_t>(i)].note == e.data1 && chord_[static_cast<size_t>(i)].down) idx = i;
    if (idx < 0) {
        out.push(e); // not ours (played before the arp was inserted): let the instrument release it
        return;
    }
    if (p_.latch) {
        chord_[static_cast<size_t>(idx)].down = false;
        return;
    }
    for (int i = idx; i + 1 < chordCount_; ++i) chord_[static_cast<size_t>(i)] = chord_[static_cast<size_t>(i + 1)];
    --chordCount_;
    stopIfEmpty();
}

int Arpeggiator::buildSequence(std::array<Entry, kMaxSeq>& seq) const {
    std::array<Entry, kMaxNotes> base{};
    for (int i = 0; i < chordCount_; ++i) base[static_cast<size_t>(i)] = chord_[static_cast<size_t>(i)];
    const bool byOrder = p_.mode == AsPlayed;
    // Insertion sort (≤ 16 entries): by press order or by pitch.
    for (int i = 1; i < chordCount_; ++i) {
        const Entry x = base[static_cast<size_t>(i)];
        int j = i;
        while (j > 0 && (byOrder ? base[static_cast<size_t>(j - 1)].order > x.order : base[static_cast<size_t>(j - 1)].note > x.note)) {
            base[static_cast<size_t>(j)] = base[static_cast<size_t>(j - 1)];
            --j;
        }
        base[static_cast<size_t>(j)] = x;
    }
    int m = 0;
    for (int o = 0; o < p_.octaves; ++o)
        for (int i = 0; i < chordCount_; ++i) {
            const int note = base[static_cast<size_t>(i)].note + 12 * o;
            if (note > 127) continue;
            Entry x = base[static_cast<size_t>(i)];
            x.note = static_cast<uint8_t>(note);
            seq[static_cast<size_t>(m++)] = x;
        }
    return m;
}

void Arpeggiator::fireStep(double exact, MidiEventBuffer& out) {
    const int off = std::min(fireSample(exact), std::max(0, n_ - 1));
    const int64_t at = t0_ + off;
    // Legato notes end at this step (before its note-on).
    for (int i = 0; i < soundingCount_;) {
        Sounding& s = sounding_[static_cast<size_t>(i)];
        if (s.legato) {
            emitOff(out, at, s.note, s.channel);
            sounding_[static_cast<size_t>(i)] = sounding_[static_cast<size_t>(--soundingCount_)];
        } else {
            ++i;
        }
    }
    const double len = grid_ ? gridBeats_ * spp_ : ownLen_;
    const bool legato = p_.gate >= 0.999f;
    const double offExact = exact + (legato ? 1.0 : static_cast<double>(p_.gate)) * len;
    int64_t offAt = t0_ + fireSample(offExact);
    if (offAt <= at) offAt = at + 1;

    std::array<Entry, kMaxSeq> seq{};
    const int m = buildSequence(seq);
    std::array<Entry, kMaxNotes> play{};
    int np = 0;
    if (m > 0) {
        if (p_.mode == Chord) {
            // All held notes (pitch order: the first chordCount_ entries of the octave-0 run) one octave per step.
            const int oct = static_cast<int>(stepCount_ % p_.octaves);
            for (int i = 0; i < chordCount_ && i < m && np < kMaxNotes; ++i) {
                Entry x = seq[static_cast<size_t>(i)];
                const int note = x.note + 12 * oct;
                if (note > 127) continue;
                x.note = static_cast<uint8_t>(note);
                play[static_cast<size_t>(np++)] = x;
            }
        } else {
            int idx = 0;
            const int64_t s = stepCount_;
            switch (p_.mode) {
            case Down: idx = m - 1 - static_cast<int>(s % m); break;
            case UpDown: {
                const int period = m > 1 ? 2 * m - 2 : 1;
                const int ph = static_cast<int>(s % period);
                idx = ph < m ? ph : period - ph;
                break;
            }
            case Random: {
                idx = static_cast<int>(rng_.nextBits() % static_cast<uint32_t>(m));
                if (m > 1 && idx == lastRandom_)
                    idx = (idx + 1 + static_cast<int>(rng_.nextBits() % static_cast<uint32_t>(m - 1))) % m;
                lastRandom_ = idx;
                break;
            }
            default: idx = static_cast<int>(s % m); break; // Up, As played
            }
            play[static_cast<size_t>(np++)] = seq[static_cast<size_t>(idx)];
        }
    }
    int vel = 100;
    const bool accent = (stepCount_ % std::max(1, p_.accentEvery)) == 0;
    for (int i = 0; i < np; ++i) {
        const Entry& x = play[static_cast<size_t>(i)];
        switch (p_.velMode) {
        case VelFixed: vel = p_.velocity; break;
        case VelAccent: vel = accent ? 127 : p_.velocity; break;
        default: vel = std::max<int>(1, x.vel); break;
        }
        // The same note still sounding (overlap through swing / chord mode): end it first.
        for (int k = 0; k < soundingCount_; ++k)
            if (sounding_[static_cast<size_t>(k)].note == x.note) {
                emitOff(out, at, x.note, sounding_[static_cast<size_t>(k)].channel);
                sounding_[static_cast<size_t>(k)] = sounding_[static_cast<size_t>(--soundingCount_)];
                break;
            }
        if (soundingCount_ >= kMaxSounding) { // full: end the oldest
            emitOff(out, at, sounding_[0].note, sounding_[0].channel);
            sounding_[0] = sounding_[static_cast<size_t>(--soundingCount_)];
        }
        out.push(MidiEvent::noteOn(x.note, vel, x.channel, static_cast<uint32_t>(off)));
        sounding_[static_cast<size_t>(soundingCount_++)] = {x.note, x.channel, offAt, legato};
    }
    ++stepCount_;
    lastStepExact_ = exact;
    if (grid_) ++nextGrid_;
    else ++nextOwn_;
}

void Arpeggiator::advance(int64_t until, MidiEventBuffer& out) {
    for (int guard = 0; guard < kMaxEventsPerAdvance; ++guard) {
        int64_t offAt = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < soundingCount_; ++i) offAt = std::min(offAt, sounding_[static_cast<size_t>(i)].offAt);
        const double stepExact = nextStepExact();
        const int64_t stepAt = stepExact == kNever ? std::numeric_limits<int64_t>::max() : t0_ + fireSample(stepExact);
        if (offAt >= until && stepAt >= until) return;
        if (offAt <= stepAt) {
            for (int i = 0; i < soundingCount_;) {
                Sounding& s = sounding_[static_cast<size_t>(i)];
                if (s.offAt == offAt) {
                    emitOff(out, offAt, s.note, s.channel);
                    sounding_[static_cast<size_t>(i)] = sounding_[static_cast<size_t>(--soundingCount_)];
                } else {
                    ++i;
                }
            }
        } else {
            fireStep(stepExact, out);
        }
    }
}

void Arpeggiator::processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext& ctx) {
    const ParamSet& ps = params();
    p_.mode = static_cast<int>(ps.get(Mode));
    p_.octaves = std::clamp(static_cast<int>(ps.get(Octaves)), 1, 4);
    p_.sync = static_cast<int>(ps.get(Sync));
    p_.rate = std::max(0.01f, ps.get(Rate));
    p_.gate = std::clamp(ps.get(Gate), 0.05f, 1.0f);
    p_.swing = std::clamp(ps.get(Swing), 0.0f, 1.0f);
    const bool latchWas = p_.latch;
    p_.latch = ps.get(Latch) >= 0.5f;
    p_.retrigger = ps.get(Retrigger) >= 0.5f;
    p_.velMode = static_cast<int>(ps.get(VelMode));
    p_.velocity = std::clamp(static_cast<int>(ps.get(Velocity)), 1, 127);
    p_.accentEvery = std::clamp(static_cast<int>(ps.get(AccentEvery)), 2, 16);

    t0_ = ctx.sampleTime;
    n_ = ctx.numSamples;
    const double tempo = ctx.transport.tempo > 1.0 ? ctx.transport.tempo : 120.0;
    spp_ = 60.0 * sr_ / tempo;
    ppq0_ = ctx.transport.ppqPosition;
    const double beats = dsp::noteDivisionBeats(p_.sync);
    gridBeats_ = beats;
    grid_ = ctx.transport.playing && beats > 0.0;
    stepLen_ = beats > 0.0 ? beats * spp_ : sr_ / static_cast<double>(p_.rate);

    // Latch turned off: drop latched notes whose keys are up.
    if (latchWas && !p_.latch) {
        int w = 0;
        for (int i = 0; i < chordCount_; ++i)
            if (chord_[static_cast<size_t>(i)].down) chord_[static_cast<size_t>(w++)] = chord_[static_cast<size_t>(i)];
        chordCount_ = w;
        stopIfEmpty();
    }

    // Clock bookkeeping.
    if (running_) {
        if (grid_ != lastGrid_) {
            if (grid_) {
                startClock(t0_, false); // next grid point from here
            } else {
                const double base = lastStepExact_ >= 0.0 ? lastStepExact_ + stepLen_ : static_cast<double>(t0_);
                anchorTime_ = std::max(base, static_cast<double>(t0_));
                anchorStep_ = nextOwn_;
                ownLen_ = stepLen_;
            }
        } else if (grid_) {
            if (gridBeats_ != gridBeatsUsed_ || std::fabs(ppq0_ - expectPpq_) > 1e-6) startClock(t0_, false);
        } else if (std::fabs(stepLen_ - ownLen_) > 1e-9 * std::max(1.0, ownLen_)) {
            // Re-anchor at the last fired step (unswung position) so the new length applies from there.
            if (nextOwn_ > anchorStep_) {
                anchorTime_ += static_cast<double>(nextOwn_ - 1 - anchorStep_) * ownLen_;
                anchorStep_ = nextOwn_ - 1;
            }
            ownLen_ = stepLen_;
        }
        if (!grid_) {
            // After a re-anchor with a shorter step the next steps may lie in the past: skip them (no burst of
            // catch-up notes at offset 0), keeping the grid and the swing parity.
            const double next = anchorTime_ + static_cast<double>(nextOwn_ - anchorStep_) * ownLen_;
            if (ownLen_ > 0.0 && next < static_cast<double>(t0_) - 1.0) { // > 1 sample late (not boundary rounding)
                const int64_t k = static_cast<int64_t>(std::ceil((static_cast<double>(t0_) - next) / ownLen_));
                anchorTime_ = next + static_cast<double>(k) * ownLen_;
                nextOwn_ += k;
                anchorStep_ = nextOwn_;
            }
        }
    }
    lastGrid_ = grid_;
    gridBeatsUsed_ = gridBeats_;
    expectPpq_ = ppq0_ + static_cast<double>(n_) / spp_;

    for (const MidiEvent& e : in) {
        const int64_t at = t0_ + static_cast<int64_t>(e.sampleOffset);
        advance(at, out);
        switch (e.type) {
        case MidiEventType::NoteOn: noteOn(e, at, out); break;
        case MidiEventType::NoteOff: noteOff(e, out); break;
        case MidiEventType::AllNotesOff:
        case MidiEventType::AllSoundOff:
            releaseAll(out, at);
            out.push(e);
            break;
        default: out.push(e); break;
        }
    }
    advance(t0_ + n_, out);
}

} // namespace ks
