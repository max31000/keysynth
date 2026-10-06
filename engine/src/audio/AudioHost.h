#pragma once
// AudioHost (ARCHITECTURE §6): wraps juce::AudioDeviceManager. Default device: ASIO matching "Steinberg"/"Yamaha"
// if present, else the system default. Type/name/sample rate/buffer size persisted in userdata/settings.json and
// switchable at runtime. Device (re)start -> Engine::prepare + full graph rebuild (onPrepared) on the message
// thread, while no callback runs; then (async, message thread) settings are saved and onChanged re-reports the
// device. openControlPanel shows the ASIO driver panel (deferred to the message loop; may be modal).

#include "audio/AudioControl.h"
#include "core/AppPaths.h"
#include "core/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <optional>

namespace ks {

class AudioHost final : public AudioControl, private juce::AudioIODeviceCallback, private juce::AsyncUpdater {
public:
    struct Options {
        std::optional<int> bufferSize;   // --asio-buffer
        std::optional<double> sampleRate;
        std::string preferType;          // default "ASIO"
    };

    // onPrepared: called after Engine::prepare() (message thread, device stopped) to rebuild + publish a graph.
    AudioHost(Engine& engine, AppPaths paths, std::function<void()> onPrepared);
    ~AudioHost() override;

    // Opens the device. Returns "" on success, else an error (engine may still be running on a fallback device).
    std::string start(const Options& opt);
    void stop();

    AudioStatus status() const override;
    AudioDeviceList listDevices() override;
    std::string setDevice(const std::string& type, const std::string& name, double sampleRate, int bufferSize) override;
    uint64_t xruns() const override;
    std::string openControlPanel() override;

    juce::AudioDeviceManager& deviceManager() noexcept { return dm_; }
    // Set once by the audio thread after its first callback.
    bool mmcssActive() const noexcept { return mmcss_.load(); }
    bool powerThrottlingDisabled() const noexcept { return powerOff_.load(); }
    unsigned long mmcssError() const noexcept { return mmcssError_.load(); }

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext& ctx) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& message) override;

    void handleAsyncUpdate() override;
    void showControlPanelNow();
    void saveSettings() const;
    nlohmann::json loadSettings() const;

    Engine& engine_;
    AppPaths paths_;
    std::function<void()> onPrepared_;
    juce::AudioDeviceManager dm_;
    std::atomic<bool> running_{false};
    std::atomic<bool> promoted_{false};
    std::atomic<bool> mmcss_{false}, powerOff_{false};
    std::atomic<unsigned long> mmcssError_{0};
    std::atomic<uint64_t> xrunBase_{0};
    std::atomic<bool> panelRequested_{false}, changed_{false};
};

} // namespace ks
