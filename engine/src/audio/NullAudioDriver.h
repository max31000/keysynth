#pragma once
// `--no-audio`: a timer thread drives Engine::process in real time without a device (ARCHITECTURE §6).

#include "audio/AudioControl.h"
#include "core/Engine.h"

#include <atomic>
#include <functional>
#include <thread>

namespace ks {

class NullAudioDriver final : public AudioControl {
public:
    NullAudioDriver(Engine& engine, std::function<void()> onPrepared, double sampleRate = 48000.0, int bufferSize = 256);
    ~NullAudioDriver() override;

    void start(); // prepares the engine (calls onPrepared) then starts the thread
    void stop();

    AudioStatus status() const override;
    AudioDeviceList listDevices() override;
    std::string setDevice(const std::string& type, const std::string& name, double sampleRate, int bufferSize) override;
    std::string restart() override;

private:
    void run();
    Engine& engine_;
    std::function<void()> onPrepared_;
    double sampleRate_;
    int bufferSize_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace ks
