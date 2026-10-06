// OfflineRenderer::loadMidiFile — Standard MIDI File -> timed events (all tracks merged, tempo map applied).
#include "render/OfflineRenderer.h"

#include "core/AppPaths.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace ks {

bool OfflineRenderer::loadMidiFile(const std::string& path, std::vector<TimedEvent>& out, std::string& error) {
    const juce::String jpath = juce::String::fromUTF8(path.c_str());
    const juce::File file = juce::File::isAbsolutePath(jpath) ? juce::File(jpath)
                                                              : juce::File::getCurrentWorkingDirectory().getChildFile(jpath);
    if (!file.existsAsFile()) {
        error = "MIDI file not found: " + path;
        return false;
    }
    juce::FileInputStream in(file);
    if (!in.openedOk()) {
        error = "cannot open " + path;
        return false;
    }
    juce::MidiFile mf;
    int format = 0;
    if (!mf.readFrom(in, true, &format)) {
        error = "not a valid MIDI file: " + path;
        return false;
    }
    mf.convertTimestampTicksToSeconds();
    for (int t = 0; t < mf.getNumTracks(); ++t) {
        const juce::MidiMessageSequence* seq = mf.getTrack(t);
        for (int i = 0; i < seq->getNumEvents(); ++i) {
            const juce::MidiMessage& m = seq->getEventPointer(i)->message;
            MidiEvent e;
            if (MidiEvent::fromBytes(m.getRawData(), m.getRawDataSize(), e))
                out.push_back({std::max(0.0, m.getTimeStamp()), e});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.time < b.time; });
    return true;
}

} // namespace ks
