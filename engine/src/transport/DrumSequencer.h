#pragma once
// DrumSequencer (Phase 3): 16-step x N tracks, patterns in presets/patterns/*.json, drives the RhythmNode's
// `drums` module. Interface stub only — no events are generated yet.

#include "core/MidiEvent.h"
#include "core/ProcessContext.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace ks {

class DrumSequencer {
public:
    virtual ~DrumSequencer() = default;
    // control thread: load a pattern (JSON per presets/patterns schema, TBD). Returns false = not implemented.
    virtual bool loadPattern(const nlohmann::json&) { return false; }
    // audio thread: append note events for this block (sample offsets within the block). Stub: nothing.
    virtual void generate(const TransportInfo&, int /*numSamples*/, double /*sampleRate*/,
                          std::vector<MidiEvent>& /*out*/) noexcept {}
};

} // namespace ks
