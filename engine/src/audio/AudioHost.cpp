#include "audio/AudioHost.h"

#include "core/RtCheck.h"
#include "platform/ThreadPriority.h"

#include <fstream>
#include <sstream>

namespace ks {

nlohmann::json toJson(const AudioStatus& s) {
    return {{"type", s.type},
            {"name", s.name},
            {"sampleRate", s.sampleRate},
            {"bufferSize", s.bufferSize},
            {"inputLatencyMs", s.inputLatencyMs},
            {"outputLatencyMs", s.outputLatencyMs},
            {"running", s.running}};
}

namespace {
std::string str(const juce::String& s) { return s.toStdString(); }
juce::String jstr(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

bool preferredAsioName(const juce::String& n) {
    return n.containsIgnoreCase("Steinberg") || n.containsIgnoreCase("Yamaha");
}
} // namespace

AudioHost::AudioHost(Engine& engine, AppPaths paths, std::function<void()> onPrepared)
    : engine_(engine), paths_(std::move(paths)), onPrepared_(std::move(onPrepared)) {}

AudioHost::~AudioHost() { stop(); }

nlohmann::json AudioHost::loadSettings() const {
    std::ifstream f(paths_.settingsFile, std::ios::binary);
    if (!f) return nlohmann::json::object();
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

void AudioHost::saveSettings() const {
    nlohmann::json j = loadSettings();
    const AudioStatus s = status();
    if (s.type.empty() || s.type == "None") return;
    j["audio"] = {{"type", s.type}, {"name", s.name}, {"sampleRate", s.sampleRate}, {"bufferSize", s.bufferSize}};
    std::error_code ec;
    std::filesystem::create_directories(paths_.userdata, ec);
    std::ofstream f(paths_.settingsFile, std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(2) << '\n';
}

std::string AudioHost::start(const Options& opt) {
    dm_.addAudioCallback(this);
    const auto& types = dm_.getAvailableDeviceTypes(); // creates + scans device types

    // 1) saved settings, 2) ASIO Steinberg/Yamaha, 3) system default.
    std::string type, name;
    double sr = 0.0;
    int buffer = 0;
    const nlohmann::json settings = loadSettings();
    if (auto a = settings.find("audio"); a != settings.end() && a->is_object()) {
        type = a->value("type", std::string());
        name = a->value("name", std::string());
        sr = a->value("sampleRate", 0.0);
        buffer = a->value("bufferSize", 0);
    }
    const std::string preferType = opt.preferType.empty() ? "ASIO" : opt.preferType;
    if (type.empty()) {
        for (auto* t : types) {
            if (str(t->getTypeName()) != preferType) continue;
            t->scanForDevices();
            for (const auto& n : t->getDeviceNames(false)) {
                if (preferType != "ASIO" || preferredAsioName(n)) {
                    type = preferType;
                    name = str(n);
                    break;
                }
            }
        }
    }
    if (opt.bufferSize) buffer = *opt.bufferSize;
    if (opt.sampleRate) sr = *opt.sampleRate;

    juce::String err;
    if (!type.empty()) {
        juce::XmlElement xml("DEVICESETUP");
        xml.setAttribute("deviceType", jstr(type));
        xml.setAttribute("audioOutputDeviceName", jstr(name));
        xml.setAttribute("audioInputDeviceName", "");
        if (sr > 0) xml.setAttribute("audioDeviceRate", sr);
        if (buffer > 0) xml.setAttribute("audioDeviceBufferSize", buffer);
        xml.setAttribute("audioDeviceOutChans", "11");
        xml.setAttribute("audioDeviceInChans", "0");
        err = dm_.initialise(0, 2, &xml, true);
    } else {
        err = dm_.initialise(0, 2, nullptr, true);
        if (err.isEmpty() && (buffer > 0 || sr > 0)) {
            auto setup = dm_.getAudioDeviceSetup();
            if (buffer > 0) setup.bufferSize = buffer;
            if (sr > 0) setup.sampleRate = sr;
            err = dm_.setAudioDeviceSetup(setup, true);
        }
    }
    if (dm_.getCurrentAudioDevice() == nullptr && err.isEmpty()) err = "no audio device could be opened";
    if (dm_.getCurrentAudioDevice() != nullptr) saveSettings();
    return str(err);
}

void AudioHost::stop() {
    dm_.removeAudioCallback(this);
    dm_.closeAudioDevice();
    running_.store(false);
}

AudioStatus AudioHost::status() const {
    AudioStatus s;
    auto* dev = dm_.getCurrentAudioDevice();
    if (dev == nullptr) {
        s.type = str(dm_.getCurrentAudioDeviceType());
        return s;
    }
    s.type = str(dev->getTypeName());
    s.name = str(dev->getName());
    s.sampleRate = dev->getCurrentSampleRate();
    s.bufferSize = dev->getCurrentBufferSizeSamples();
    if (s.sampleRate > 0) {
        s.inputLatencyMs = 1000.0 * dev->getInputLatencyInSamples() / s.sampleRate;
        s.outputLatencyMs = 1000.0 * dev->getOutputLatencyInSamples() / s.sampleRate;
    }
    s.running = running_.load() && dev->isPlaying();
    return s;
}

AudioDeviceList AudioHost::listDevices() {
    AudioDeviceList l;
    for (auto* t : dm_.getAvailableDeviceTypes()) {
        l.types.push_back(str(t->getTypeName()));
        t->scanForDevices();
        AudioDeviceList::TypeDevices td;
        td.type = str(t->getTypeName());
        for (const auto& n : t->getDeviceNames(false)) td.names.push_back(str(n));
        if (auto* dev = dm_.getCurrentAudioDevice(); dev != nullptr && dev->getTypeName() == t->getTypeName()) {
            for (double r : dev->getAvailableSampleRates()) td.sampleRates.push_back(r);
            for (int b : dev->getAvailableBufferSizes()) td.bufferSizes.push_back(b);
        }
        l.available.push_back(std::move(td));
    }
    return l;
}

std::string AudioHost::setDevice(const std::string& type, const std::string& name, double sampleRate, int bufferSize) {
    bool typeOk = false;
    for (auto* t : dm_.getAvailableDeviceTypes()) typeOk = typeOk || str(t->getTypeName()) == type;
    if (!typeOk) return "unknown device type '" + type + "'";
    if (str(dm_.getCurrentAudioDeviceType()) != type) dm_.setCurrentAudioDeviceType(jstr(type), true);
    auto setup = dm_.getAudioDeviceSetup();
    setup.outputDeviceName = jstr(name);
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    setup.useDefaultOutputChannels = true;
    if (sampleRate > 0) setup.sampleRate = sampleRate;
    if (bufferSize > 0) setup.bufferSize = bufferSize;
    const juce::String err = dm_.setAudioDeviceSetup(setup, true);
    if (err.isEmpty()) saveSettings();
    return str(err);
}

uint64_t AudioHost::xruns() const {
    auto* dev = dm_.getCurrentAudioDevice();
    const int x = dev ? dev->getXRunCount() : 0;
    return (x > 0 ? static_cast<uint64_t>(x) : 0) + engine_.telemetry().overloads.load();
}

void AudioHost::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    // Message thread (JUCE opens/restarts devices there); no callback is running.
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    engine_.prepare(sr, bs > 0 ? bs : 512);
    if (onPrepared_) onPrepared_();
    promoted_.store(false);
    running_.store(true);
}

void AudioHost::audioDeviceStopped() { running_.store(false); }

void AudioHost::audioDeviceError(const juce::String&) { running_.store(false); }

void AudioHost::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* out, int numOut,
                                                 int numSamples, const juce::AudioIODeviceCallbackContext&) {
    if (!promoted_.load(std::memory_order_relaxed)) {
        const auto info = platform::promoteCurrentThreadForAudio(); // one-shot, before entering the RT scope
        mmcss_.store(info.mmcss);
        mmcssError_.store(info.errorCode);
        powerOff_.store(info.powerThrottlingOff);
        promoted_.store(true, std::memory_order_relaxed);
    }
    engine_.process(out, numOut, numSamples);
}

} // namespace ks
