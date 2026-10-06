#include "audio/NullAudioDriver.h"

#include <chrono>
#include <vector>

namespace ks {

NullAudioDriver::NullAudioDriver(Engine& engine, std::function<void()> onPrepared, double sampleRate, int bufferSize)
    : engine_(engine), onPrepared_(std::move(onPrepared)), sampleRate_(sampleRate), bufferSize_(bufferSize) {}

NullAudioDriver::~NullAudioDriver() { stop(); }

void NullAudioDriver::start() {
    stop();
    engine_.prepare(sampleRate_, bufferSize_);
    if (onPrepared_) onPrepared_();
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void NullAudioDriver::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void NullAudioDriver::run() {
    std::vector<float> l(static_cast<size_t>(bufferSize_)), r(static_cast<size_t>(bufferSize_));
    float* outs[2] = {l.data(), r.data()};
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(bufferSize_ / sampleRate_));
    auto next = clock::now();
    while (running_.load(std::memory_order_relaxed)) {
        engine_.process(outs, 2, bufferSize_);
        next += period;
        const auto now = clock::now();
        if (next < now - period * 8) next = now; // fell far behind (debugger etc.): resync
        std::this_thread::sleep_until(next);
    }
}

AudioStatus NullAudioDriver::status() const {
    AudioStatus s;
    s.type = "None";
    s.name = "Null (no audio)";
    s.sampleRate = sampleRate_;
    s.bufferSize = bufferSize_;
    s.outputLatencyMs = 1000.0 * bufferSize_ / sampleRate_;
    s.running = running_.load();
    return s;
}

AudioDeviceList NullAudioDriver::listDevices() {
    AudioDeviceList l;
    l.types = {"None"};
    AudioDeviceList::TypeDevices td;
    td.type = "None";
    td.names = {"Null (no audio)"};
    td.sampleRates = {44100.0, 48000.0, 96000.0};
    td.bufferSizes = {32, 64, 128, 256, 512, 1024};
    l.available = {td};
    return l;
}

std::string NullAudioDriver::setDevice(const std::string& type, const std::string&, double sampleRate, int bufferSize) {
    if (type != "None") return "audio is disabled (--no-audio)";
    const bool wasRunning = running_.load();
    stop();
    if (sampleRate > 0) sampleRate_ = sampleRate;
    if (bufferSize > 0) bufferSize_ = bufferSize;
    if (wasRunning) start();
    return {};
}

} // namespace ks
