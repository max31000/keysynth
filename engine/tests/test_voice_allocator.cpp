#include "core/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>

using namespace ks;

namespace {
struct FakeVoice {
    bool active = false;
    bool releasing = false;
    int note = -1;
    int starts = 0;
    bool lastLegato = false;
    int lastGlideFrom = -1;
    bool killed = false;
    void noteOn(const VoiceStart& s) {
        active = true;
        releasing = false;
        note = s.note;
        ++starts;
        lastLegato = s.legato;
        lastGlideFrom = s.glideFrom;
        killed = false;
    }
    void noteOff() { releasing = true; }
    void kill() { killed = true; }
    void reset() { active = releasing = false; }
    bool isActive() const { return active; }
};

using Alloc = VoiceAllocator<FakeVoice, 4>;

int countSounding(Alloc& a, int note) {
    int c = 0;
    for (int i = 0; i < 4; ++i)
        if (a.voice(i).active && !a.voice(i).releasing && a.voice(i).note == note) ++c;
    return c;
}
int findVoice(Alloc& a, int note) {
    for (int i = 0; i < 4; ++i)
        if (a.voice(i).active && a.voice(i).note == note) return i;
    return -1;
}
} // namespace

TEST_CASE("VoiceAllocator: poly allocation and release", "[voice]") {
    Alloc a;
    a.noteOn(60, 1.0f);
    a.noteOn(64, 1.0f);
    REQUIRE(a.activeCount() == 2);
    a.noteOff(60);
    REQUIRE(a.voice(findVoice(a, 60)).releasing);
    REQUIRE_FALSE(a.voice(findVoice(a, 64)).releasing);
}

TEST_CASE("VoiceAllocator: stealing prefers released voices, then oldest", "[voice]") {
    Alloc a;
    for (int n : {60, 62, 64, 65}) a.noteOn(n, 1.0f);
    a.noteOff(62); // 62 releasing (still active)
    a.noteOn(67, 1.0f);
    REQUIRE(findVoice(a, 62) == -1); // released voice was stolen
    REQUIRE(findVoice(a, 67) >= 0);
    REQUIRE(a.voice(findVoice(a, 67)).killed == false);

    // All held now: oldest (60) is stolen.
    a.noteOn(69, 1.0f);
    REQUIRE(findVoice(a, 60) == -1);
    REQUIRE(findVoice(a, 69) >= 0);
    REQUIRE(a.activeCount() == 4);
}

TEST_CASE("VoiceAllocator: re-striking a sounding note reuses its voice", "[voice]") {
    Alloc a;
    a.noteOn(60, 1.0f);
    a.noteOff(60);
    a.noteOn(60, 0.5f);
    REQUIRE(a.activeCount() == 1);
    REQUIRE(countSounding(a, 60) == 1);
}

TEST_CASE("VoiceAllocator: sustain pedal holds released notes", "[voice]") {
    Alloc a;
    a.setSustain(true);
    a.noteOn(60, 1.0f);
    a.noteOff(60);
    const int v = findVoice(a, 60);
    REQUIRE_FALSE(a.voice(v).releasing);
    REQUIRE(a.isSustained(v));
    a.noteOn(64, 1.0f); // held while pedal down
    a.setSustain(false);
    REQUIRE(a.voice(v).releasing);
    REQUIRE_FALSE(a.voice(findVoice(a, 64)).releasing); // still physically held
}

TEST_CASE("VoiceAllocator: sostenuto latches only notes held at pedal-down", "[voice]") {
    Alloc a;
    a.noteOn(48, 1.0f);
    a.setSostenuto(true);
    a.noteOn(60, 1.0f);
    a.noteOff(48);
    a.noteOff(60);
    REQUIRE_FALSE(a.voice(findVoice(a, 48)).releasing); // latched
    REQUIRE(a.voice(findVoice(a, 60)).releasing);       // not latched
    a.setSostenuto(false);
    REQUIRE(a.voice(findVoice(a, 48)).releasing);
}

TEST_CASE("VoiceAllocator: all notes off ignores pedals", "[voice]") {
    Alloc a;
    a.setSustain(true);
    a.noteOn(60, 1.0f);
    a.noteOff(60);
    a.allNotesOff();
    REQUIRE(a.voice(findVoice(a, 60)).releasing);
}

TEST_CASE("VoiceAllocator: mono last-note priority and legato", "[voice]") {
    Alloc a;
    a.setMode(VoiceMode::Legato);
    a.noteOn(60, 1.0f);
    REQUIRE(a.voice(0).note == 60);
    REQUIRE_FALSE(a.voice(0).lastLegato);
    a.noteOn(64, 1.0f);
    REQUIRE(a.voice(0).note == 64);
    REQUIRE(a.voice(0).lastLegato);           // no retrigger
    REQUIRE(a.voice(0).lastGlideFrom == 60);  // glide hook
    a.noteOff(64);                            // back to 60
    REQUIRE(a.voice(0).note == 60);
    REQUIRE_FALSE(a.voice(0).releasing);
    a.noteOff(60);
    REQUIRE(a.voice(0).releasing);
    REQUIRE(a.activeCount() == 1);

    Alloc m;
    m.setMode(VoiceMode::Mono);
    m.noteOn(60, 1.0f);
    m.noteOn(62, 1.0f);
    REQUIRE_FALSE(m.voice(0).lastLegato); // mono retriggers
    REQUIRE(m.voice(0).starts == 2);
}

TEST_CASE("VoiceAllocator: polyphony limit", "[voice]") {
    Alloc a;
    a.setPolyphony(2);
    for (int n : {60, 62, 64}) a.noteOn(n, 1.0f);
    REQUIRE(a.activeCount() == 2);
    REQUIRE(findVoice(a, 60) == -1);
}
