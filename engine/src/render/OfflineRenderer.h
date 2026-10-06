#pragma once
// OfflineRenderer: patch + timed events (or a MIDI file) -> stereo buffer / WAV, without an audio device.
// Uses the real Engine (GraphBuilder, GraphSwapper, limiter), so renders match live output.

#include "core/MidiEvent.h"
#include "core/ModuleRegistry.h"
#include "core/Patch.h"
#include "transport/Pattern.h"

#include <string>
#include <vector>

namespace ks {

struct TimedEvent {
    double time = 0.0; // seconds
    MidiEvent event;
};

struct NoteSpec {
    int note = 60;
    double start = 0.0;    // s
    double duration = 0.5; // s
    int velocity = 100;
};

struct RenderOptions {
    double sampleRate = 48000.0;
    int blockSize = 64;
    double tailSeconds = 2.0; // rendered after the last event
    // Optional drum pattern: transport plays from t = 0 for `patternBars` bars (pattern tempo/kit/swing applied,
    // `tempo` > 0 overrides), then stops; the tail follows.
    const Pattern* pattern = nullptr;
    int patternBars = 1;
    double tempo = 0.0;
    int readyTimeoutSeconds = 300; // max wait for modules to finish loading (Module::isReady)
};

struct RenderResult {
    double sampleRate = 48000.0;
    std::vector<float> left, right;
    std::vector<std::string> warnings;
    double seconds() const { return sampleRate > 0 ? static_cast<double>(left.size()) / sampleRate : 0.0; }
};

class OfflineRenderer {
public:
    static RenderResult render(const Patch& patch, std::vector<TimedEvent> events, const RenderOptions& opt,
                               const ModuleRegistry& registry = defaultRegistry());

    static std::vector<TimedEvent> notesToEvents(const std::vector<NoteSpec>& notes, int channel = 1);
    // "C4:0:1[:100],E4:0:1,..."  (note name or number : start s : duration s [: velocity 1..127])
    static bool parseNotes(const std::string& spec, std::vector<NoteSpec>& out, std::string& error);
    // Controller automation, appended to `out` (unsorted; render() sorts):
    //   "CC:VALUE:TIME[:RAMP],..."  CC 0..119 or mod / breath / expr / sustain; VALUE 0..127; TIME s.
    //   RAMP > 0 s: linear ramp from the controller's previous value (0, expr 127) starting at TIME, reaching
    //   VALUE at TIME + RAMP (one event per ~5 ms).
    static bool parseControllers(const std::string& spec, std::vector<TimedEvent>& out, std::string& error,
                                 int channel = 1);
    //   "VALUE:TIME[:RAMP],..."  pitch bend VALUE -1..1 (full range = the patch's bend range), ramps as above.
    static bool parseBends(const std::string& spec, std::vector<TimedEvent>& out, std::string& error,
                           int channel = 1);
    // "C4" = 60, "F#3", "Bb2", "60". Returns -1 on error.
    static int parseNoteName(const std::string& s);
    // Standard test pattern: chord, scale, sustain-pedal section (used by render tests and ks-bench).
    static std::vector<TimedEvent> standardTestEvents();

    static bool loadMidiFile(const std::string& path, std::vector<TimedEvent>& out, std::string& error);
    // 32-bit float stereo WAV.
    static bool writeWav(const std::string& path, const RenderResult& r, std::string& error);
};

} // namespace ks
