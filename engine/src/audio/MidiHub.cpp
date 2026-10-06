#include "audio/MidiHub.h"

#include <algorithm>

namespace ks {

MidiHub::~MidiHub() { closeAll(); }

void MidiHub::Input::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) {
    MidiEvent e;
    if (MidiEvent::fromBytes(m.getRawData(), m.getRawDataSize(), e)) engine->midiSource(slot).push(e); // drop if full
}

int MidiHub::freeSlot() const {
    for (int s = 0; s < Engine::kMaxMidiSources; ++s) {
        const bool used = std::any_of(inputs_.begin(), inputs_.end(), [&](const auto& i) { return i->slot == s; });
        if (!used) return s;
    }
    return -1;
}

void MidiHub::rescan() {
    const auto available = juce::MidiInput::getAvailableDevices();
    // Close vanished devices.
    for (auto it = inputs_.begin(); it != inputs_.end();) {
        const bool present = std::any_of(available.begin(), available.end(), [&](const juce::MidiDeviceInfo& d) {
            return d.identifier.toStdString() == (*it)->identifier;
        });
        if (!present) {
            (*it)->device->stop();
            // Device thread stopped: we are the only producer now. Release its held notes.
            engine_.midiSource((*it)->slot).push(MidiEvent::allNotesOff());
            engine_.setMidiSourceActive((*it)->slot, false);
            it = inputs_.erase(it);
        } else {
            ++it;
        }
    }
    // Open new ones.
    for (const auto& d : available) {
        const std::string id = d.identifier.toStdString();
        const bool open = std::any_of(inputs_.begin(), inputs_.end(), [&](const auto& i) { return i->identifier == id; });
        if (open) continue;
        const int slot = freeSlot();
        if (slot < 0) break;
        auto in = std::make_unique<Input>();
        in->engine = &engine_;
        in->slot = slot;
        in->identifier = id;
        in->name = d.name.toStdString();
        in->device = juce::MidiInput::openDevice(d.identifier, in.get());
        if (!in->device) continue; // busy / failed: try again on next rescan
        engine_.setMidiSourceActive(slot, true);
        in->device->start();
        inputs_.push_back(std::move(in));
    }
}

std::vector<std::string> MidiHub::inputs() const {
    std::vector<std::string> out;
    for (const auto& i : inputs_) out.push_back(i->name);
    return out;
}

void MidiHub::closeAll() {
    for (auto& i : inputs_) {
        if (i->device) i->device->stop();
        engine_.setMidiSourceActive(i->slot, false);
    }
    inputs_.clear();
}

} // namespace ks
