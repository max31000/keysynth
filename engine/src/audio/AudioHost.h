#pragma once
// AudioHost (ARCHITECTURE §6): wraps juce::AudioDeviceManager. Default device: ASIO matching "Steinberg"/"Yamaha"
// if present, else the system default. Type/name/sample rate/buffer size persisted in userdata/settings.json and
// switchable at runtime. Device (re)start -> Engine::prepare + full graph rebuild (onPrepared) on the message
// thread, while no callback runs; then (async, message thread) settings are saved and onChanged re-reports the
// device. openControlPanel shows the ASIO driver panel (deferred to the message loop; may be modal).
//
// Robustness (§6 "Device restarts behind our back"): a message-thread watchdog (tick, every 100 ms) reopens the
// device with the driver's current settings when it stalls (> stallMs without callbacks), reports an error, vanishes
// or a driver-initiated restart failed. Every device event goes to userdata/logs/audio.log and (unexpected ones) to
// onLog.

#include "audio/AudioControl.h"
#include "core/AppPaths.h"
#include "core/Engine.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include "core/RtCheck.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ks {

class AudioHost final : public AudioControl,
                        private juce::AudioIODeviceCallback,
                        private juce::AsyncUpdater,
                        private juce::Timer {
public:
    struct Options {
        std::optional<int> bufferSize;   // --asio-buffer
        std::optional<double> sampleRate;
        std::string preferType;          // default "ASIO"
    };
    struct Watchdog {
        double stallMs = 500.0;       // no callbacks for this long while running -> stall (logged)
        // ... and for this long -> reopen. Longer than JUCE's own ASIO reset (500 ms timer + reopen), so a normal
        // driver-initiated buffer change is never interrupted by us.
        double recoverMs = 1500.0;
        double firstRetryMs = 1000.0; // delay after a failed recovery attempt, doubled each time
        double maxRetryMs = 30000.0;
    };

    // onPrepared: called after Engine::prepare() (message thread, device stopped) to rebuild + publish a graph.
    AudioHost(Engine& engine, AppPaths paths, std::function<void()> onPrepared);
    ~AudioHost() override;

    // Before start(): use this device type instead of JUCE's defaults (tests: a fake driver). Message thread.
    void addDeviceType(std::unique_ptr<juce::AudioIODeviceType> type);
    void setWatchdog(const Watchdog& w) { watchdog_ = w; }

    // Opens the device. Returns "" on success, else an error (engine may still be running on a fallback device).
    // startTimerNow=false: no internal 100 ms timer (tests drive tick() with their own clock).
    std::string start(const Options& opt, bool startTimerNow = true);
    void stop();

    AudioStatus status() const override;
    AudioDeviceList listDevices() override;
    std::string setDevice(const std::string& type, const std::string& name, double sampleRate, int bufferSize) override;
    uint64_t xruns() const override;
    std::string openControlPanel() override;
    std::string restart() override;

    // Message thread: watchdog + deferred reports. Called by the internal timer; public for tests (nowMs: any
    // monotonic clock in ms).
    void tick(double nowMs);

    juce::AudioDeviceManager& deviceManager() noexcept { return dm_; }
    // Set once by the audio thread after its first callback.
    bool mmcssActive() const noexcept { return mmcss_.load(); }
    bool powerThrottlingDisabled() const noexcept { return powerOff_.load(); }
    unsigned long mmcssError() const noexcept { return mmcssError_.load(); }
    uint64_t callbackCount() const noexcept { return callbacks_.load(std::memory_order_relaxed); }
    int recoveryAttempts() const noexcept { return recoveryAttempts_; } // message thread
    std::filesystem::path logFile() const { return paths_.userdata / "logs" / "audio.log"; }

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext& ctx) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& message) override;

    void handleAsyncUpdate() override;
    void timerCallback() override;
    void showControlPanelNow();
    void saveSettings() const;
    nlohmann::json loadSettings() const;
    // Close + reopen the target device with the driver's current settings (ASIO) / the last explicit ones.
    std::string reopen(const std::string& reason);
    void rememberTarget();
    // Device event -> audio.log (+ onLog when notify). Message thread only (file IO).
    void logEvent(const std::string& level, const std::string& event, const std::string& detail, bool notify,
                  juce::AudioIODevice* dev = nullptr);
    void flushNotes();

    Engine& engine_;
    AppPaths paths_;
    std::function<void()> onPrepared_;
    juce::AudioDeviceManager dm_;
    Watchdog watchdog_;
    std::atomic<bool> running_{false};
    std::atomic<bool> promoted_{false};
    std::atomic<bool> mmcss_{false}, powerOff_{false};
    std::atomic<unsigned long> mmcssError_{0};
    std::atomic<uint64_t> xrunBase_{0};
    std::atomic<bool> panelRequested_{false}, changed_{false};
    std::atomic<bool> panelOpen_{false}; // message thread: inside showControlPanelNow (see there)

    // audio thread -> message thread
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<bool> errorPending_{false}, errorWriting_{false};
    std::array<char, 256> errorText_{};
    std::atomic<bool> stoppedOffThread_{false};

    // message thread
    struct Target {
        std::string type, name;
    } target_;
    int explicitBuffer_ = 0;      // > 0: the user chose this size and the driver accepted it
    bool expectRunning_ = false;  // a device should be delivering callbacks
    int selfOps_ = 0;             // > 0 while we restart the device ourselves (not a driver restart)
    std::string selfReason_;
    bool recoverNow_ = false;     // a driver restart failed / error: recover at the next tick
    std::string recoverReason_;
    bool recovering_ = false;     // reopened after a failure; cleared once callbacks flow
    bool stallLogged_ = false;
    int recoveryAttempts_ = 0;
    double retryDelayMs_ = 0.0, nextRetryMs_ = 0.0;
    uint64_t startEpoch_ = 0, seenEpoch_ = 0, lastCount_ = 0, startCount_ = 0;
    double lastProgressMs_ = 0.0, lastTickMs_ = 0.0;
    double lastSr_ = 0.0;
    int lastBs_ = 0;
    // Defensive: JUCE device types normally call aboutToStart/stopped on the message thread, but not all are
    // guaranteed to. Never taken on the audio thread (ks::Mutex flags that).
    Mutex notesMutex_;
    struct Note {
        std::string level, msg;
        bool notify;
    };
    std::vector<Note> notes_; // onLog calls deferred out of JUCE device callbacks
    mutable Mutex logMutex_;
};

} // namespace ks
