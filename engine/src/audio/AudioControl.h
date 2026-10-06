#pragma once
// Control-thread interfaces to the audio/MIDI host, so protocol handlers can be tested without devices.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ks {

struct AudioStatus {
    std::string type;
    std::string name;
    double sampleRate = 0.0;
    int bufferSize = 0;
    double inputLatencyMs = 0.0;
    double outputLatencyMs = 0.0;
    bool running = false;
    bool hasControlPanel = false; // the driver has its own settings panel (ASIO): `open_audio_panel`
};

nlohmann::json toJson(const AudioStatus& s);

struct AudioDeviceList {
    std::vector<std::string> types;
    struct TypeDevices {
        std::string type;
        std::vector<std::string> names;
        // Supported values of the *open* device when it is of this type; empty for other types (unknown until
        // selected — probing would have to open drivers, and ASIO allows only one loaded driver).
        std::vector<double> sampleRates;
        std::vector<int> bufferSizes;
    };
    std::vector<TypeDevices> available;
};

class AudioControl {
public:
    virtual ~AudioControl() = default;
    virtual AudioStatus status() const = 0;
    virtual AudioDeviceList listDevices() = 0;
    // sampleRate/bufferSize <= 0 keep the current/default value. Returns an error message ("" = ok).
    virtual std::string setDevice(const std::string& type, const std::string& name, double sampleRate, int bufferSize) = 0;
    virtual uint64_t xruns() const { return 0; }
    // Opens the driver's own control panel (ASIO). Asynchronous: returns "" when the panel was scheduled, else an
    // error. Drivers like the Yamaha Steinberg USB ASIO only accept the buffer size chosen there; the device
    // restarts afterwards and onChanged fires.
    virtual std::string openControlPanel() { return "this audio device has no control panel"; }

    // Message thread: the device (re)started or its settings changed outside a setDevice call (driver reset
    // request after a control-panel change, panel closed). Set by the app to re-report `devices` + `state`.
    std::function<void()> onChanged;
};

class MidiControl {
public:
    virtual ~MidiControl() = default;
    virtual std::vector<std::string> inputs() const = 0;
    virtual void rescan() = 0;
};

} // namespace ks
