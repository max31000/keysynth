#pragma once
// MidiHub (ARCHITECTURE §6): opens every MIDI input; each device pushes into its own Engine SPSC slot from its
// JUCE callback thread. rescan() (startup + protocol `rescan_midi`) opens new devices and closes vanished ones.

#include "audio/AudioControl.h"
#include "core/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <vector>

namespace ks {

class MidiHub final : public MidiControl {
public:
    explicit MidiHub(Engine& engine) : engine_(engine) {}
    ~MidiHub() override;

    void rescan() override;                       // message thread
    std::vector<std::string> inputs() const override; // names of open inputs
    void closeAll();

private:
    struct Input final : juce::MidiInputCallback {
        Engine* engine = nullptr;
        int slot = -1;
        std::string identifier, name;
        std::unique_ptr<juce::MidiInput> device;
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override;
    };
    int freeSlot() const;

    Engine& engine_;
    std::vector<std::unique_ptr<Input>> inputs_;
};

} // namespace ks
