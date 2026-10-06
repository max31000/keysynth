// AudioHost robustness (ARCHITECTURE §6 "Device restarts behind our back"): a fake ASIO driver/device that changes
// its buffer size / sample rate behind our back (like the Yamaha Steinberg USB driver after a change in its own
// settings app), restarts the way JUCE's ASIOAudioIODevice::timerCallback does (including restarting callbacks
// after a FAILED reopen), stalls, and reports errors. The test thread plays the JUCE message thread.

#include "audio/AudioHost.h"
#include "core/Engine.h"
#include "core/RackGraph.h"
#include "core/RtCheck.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_events/juce_events.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace ks;

namespace {

const juce::String kDeviceName = "Fake Steinberg UR22C";

// The "hardware + driver" state shared by every device instance (a reopen creates a new instance, like a reload of
// the ASIO driver).
struct Driver {
    std::atomic<int> preferred{256};
    std::atomic<double> rate{48000.0};
    std::atomic<int> failOpens{0}; // the next N opens fail ("Can't create i/o buffers")
    std::atomic<int> opens{0};
    bool onlyPreferred = false;    // Yamaha-like: offers only its control-panel size
};

class FakeDevice final : public juce::AudioIODevice {
public:
    FakeDevice(Driver& d, const juce::String& name) : juce::AudioIODevice(name, "ASIO"), d_(d) {
        sr_ = d_.rate.load();
        bs_ = d_.preferred.load();
        l_.assign(32768, 0.0f);
        r_.assign(32768, 0.0f);
    }
    ~FakeDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return {"Out L", "Out R"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0, 96000.0}; }
    juce::Array<int> getAvailableBufferSizes() override {
        juce::Array<int> a;
        if (!d_.onlyPreferred)
            for (int s : {64, 128, 256, 512, 1024}) a.add(s);
        a.addIfNotAlreadyThere(d_.preferred.load());
        a.sort();
        return a;
    }
    int getDefaultBufferSize() override { return d_.preferred.load(); }

    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double sr, int bs) override {
        close();
        ++d_.opens;
        if (d_.failOpens.load() > 0) {
            --d_.failOpens;
            error_ = "Can't create i/o buffers";
            return error_;
        }
        error_.clear();
        if (sr > 0) d_.rate.store(sr); // the host sets the driver's rate
        sr_ = d_.rate.load();
        bs_ = getAvailableBufferSizes().contains(bs) ? bs : d_.preferred.load();
        open_ = true;
        return {};
    }
    void close() override {
        stop();
        open_ = false;
    }
    bool isOpen() override { return open_; }
    void start(juce::AudioIODeviceCallback* cb) override {
        if (cb == nullptr) return;
        cb->audioDeviceAboutToStart(this);
        {
            std::lock_guard<std::mutex> l(m_);
            cb_ = cb;
        }
        if (open_) startThread();
    }
    void stop() override {
        juce::AudioIODeviceCallback* last;
        {
            std::lock_guard<std::mutex> l(m_);
            last = cb_;
            cb_ = nullptr;
        }
        stopThread();
        if (last != nullptr) last->audioDeviceStopped();
    }
    bool isPlaying() override {
        std::lock_guard<std::mutex> l(m_);
        return cb_ != nullptr;
    }
    juce::String getLastError() override { return error_; }
    int getCurrentBufferSizeSamples() override { return bs_; }
    double getCurrentSampleRate() override { return sr_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override {
        juce::BigInteger b;
        b.setRange(0, 2, true);
        return b;
    }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return bs_ + 32; }
    int getInputLatencyInSamples() override { return bs_ + 16; }

    // --- simulations (test thread = message thread) ---
    // juce_ASIO_windows.cpp ASIOAudioIODevice::timerCallback after kAsioResetRequest / kAsioBufferSizeChange: close,
    // reopen (the driver's new preferred size), then start the old callback again — even when the reopen failed.
    // (Real JUCE passes the old size and switches to the preferred one only when it noticed the change; the fake
    // models the successful case directly.)
    void driverReset(bool reopenFails) {
        juce::AudioIODeviceCallback* old;
        {
            std::lock_guard<std::mutex> l(m_);
            old = cb_;
        }
        close();
        if (reopenFails) {
            error_ = "Device didn't start correctly";
            open_ = false;
        } else {
            open({}, {}, 0.0, d_.preferred.load());
        }
        if (old != nullptr) start(old);
    }
    void stall() { stopThread(); } // callbacks stop silently; the device still says "playing"
    void fail(const char* msg) {   // the device thread reports an error and stops
        errorMsg_ = msg;
        errorReq_.store(true);
    }

private:
    void startThread() {
        stopThread();
        run_.store(true);
        thread_ = std::thread([this] { loop(); });
    }
    void stopThread() {
        run_.store(false);
        if (thread_.joinable()) thread_.join();
    }
    void loop() {
        float* outs[2] = {l_.data(), r_.data()};
        const juce::AudioIODeviceCallbackContext ctx{};
        while (run_.load()) {
            {
                std::lock_guard<std::mutex> l(m_);
                if (cb_ != nullptr) {
                    if (errorReq_.exchange(false)) {
                        cb_->audioDeviceError(errorMsg_);
                        break;
                    }
                    cb_->audioDeviceIOCallbackWithContext(nullptr, 0, outs, 2, bs_, ctx);
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    }

    Driver& d_;
    double sr_ = 48000.0;
    int bs_ = 256;
    bool open_ = false;
    juce::String error_;
    std::mutex m_;
    juce::AudioIODeviceCallback* cb_ = nullptr;
    std::thread thread_;
    std::atomic<bool> run_{false}, errorReq_{false};
    juce::String errorMsg_;
    std::vector<float> l_, r_;
};

class FakeType final : public juce::AudioIODeviceType {
public:
    explicit FakeType(Driver& d) : juce::AudioIODeviceType("ASIO"), d_(d) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool input) const override {
        return input ? juce::StringArray() : juce::StringArray(kDeviceName);
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* d, bool input) const override { return d != nullptr && !input ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String& out, const juce::String& in) override {
        const juce::String name = out.isNotEmpty() ? out : in;
        if (name != kDeviceName) return nullptr;
        ++created;
        return new FakeDevice(d_, name);
    }
    int created = 0;

private:
    Driver& d_;
};

struct HostFixture {
    juce::ScopedJuceInitialiser_GUI juceInit; // message manager: the test thread is the message thread
    std::filesystem::path root;
    Driver driver;
    FakeType* type = nullptr;
    Engine engine;
    std::vector<std::pair<double, int>> prepares;
    std::vector<std::string> logs;
    std::unique_ptr<AudioHost> host;
    double now = 0.0;

    HostFixture() {
        static std::atomic<int> counter{0};
        root = std::filesystem::temp_directory_path() /
               ("ks-audiohost-test-" + std::to_string(juce::Random::getSystemRandom().nextInt64()) + "-" +
                std::to_string(juce::Time::getHighResolutionTicks()) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "userdata");
        makeHost();
    }
    ~HostFixture() {
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void makeHost() {
        host.reset();
        host = std::make_unique<AudioHost>(engine, AppPaths::fromRoot(root), [this] {
            prepares.emplace_back(engine.sampleRate(), engine.maxBlock());
            engine.publish(std::make_unique<RackGraph>(engine.sampleRate(), engine.maxBlock()));
        });
        auto t = std::make_unique<FakeType>(driver);
        type = t.get();
        host->addDeviceType(std::move(t));
        host->onLog = [this](const std::string& level, const std::string& msg, bool notify) {
            logs.push_back(level + (notify ? "!" : "") + ": " + msg);
        };
    }
    std::string start(const AudioHost::Options& o = {}) {
        const std::string e = host->start(o, false);
        tick(0.0);
        return e;
    }
    FakeDevice* dev() { return dynamic_cast<FakeDevice*>(host->deviceManager().getCurrentAudioDevice()); }
    bool waitCallbacks(uint64_t n = 20, int timeoutMs = 3000) {
        const uint64_t c0 = host->callbackCount();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (host->callbackCount() < c0 + n) {
            if (std::chrono::steady_clock::now() > end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    void tick(double dt) {
        now += dt;
        host->tick(now);
    }
    bool logged(const std::string& needle) const {
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    std::string logFile() const {
        std::ifstream f(host->logFile(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    nlohmann::json savedAudio() const {
        std::ifstream f(root / "userdata" / "settings.json", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        auto j = nlohmann::json::parse(ss.str(), nullptr, false);
        return j.is_object() && j.contains("audio") ? j["audio"] : nlohmann::json();
    }
};

} // namespace

TEST_CASE("audio host: driver changes buffer size (bigger, smaller) and sample rate behind our back", "[audio]") {
    HostFixture f;
    const uint64_t v0 = rt::violationCount();
    REQUIRE(f.start().empty());
    REQUIRE(f.dev() != nullptr);
    CHECK(f.host->status().bufferSize == 256);
    CHECK(f.engine.maxBlock() == 256);
    REQUIRE(f.waitCallbacks());

    struct Step {
        int buffer;
        double rate;
    };
    for (const Step s : {Step{1024, 48000.0}, Step{64, 48000.0}, Step{64, 44100.0}, Step{8192, 96000.0}}) {
        INFO("buffer " << s.buffer << " rate " << s.rate);
        const size_t prepBefore = f.prepares.size();
        f.driver.preferred = s.buffer;
        f.driver.rate = s.rate;
        f.dev()->driverReset(false); // kAsioResetRequest -> JUCE reopens with the new preferred size
        CHECK(f.engine.maxBlock() == s.buffer);
        CHECK(f.engine.sampleRate() == s.rate);
        REQUIRE(f.prepares.size() == prepBefore + 1);
        CHECK(f.prepares.back() == std::make_pair(s.rate, s.buffer));
        REQUIRE(f.waitCallbacks()); // audio continues
        f.tick(100);
        f.tick(100);
        CHECK(f.host->recoveryAttempts() == 0);
        CHECK(f.host->status().bufferSize == s.buffer);
        CHECK(f.host->status().running);
        const auto a = f.savedAudio();
        CHECK(a["bufferSize"] == s.buffer); // the new size is persisted ...
        CHECK(a["bufferExplicit"] == false); // ... as the driver's choice, not ours
    }
    CHECK(f.logged("driver restart"));
    CHECK(f.logged("buffer 256 -> 1024"));
    CHECK(f.logFile().find("[info] start") != std::string::npos);
    CHECK(rt::violationCount() == v0);
}

TEST_CASE("audio host: a failed driver restart (JUCE restarts callbacks anyway) is recovered", "[audio]") {
    HostFixture f;
    const uint64_t v0 = rt::violationCount();
    REQUIRE(f.start().empty());
    REQUIRE(f.waitCallbacks());
    f.driver.preferred = 128;
    f.dev()->driverReset(true); // reopen failed, but JUCE calls audioDeviceAboutToStart + keeps the callback
    CHECK_FALSE(f.host->status().running);
    f.tick(100); // recovered at the next tick, no stall wait
    CHECK(f.logged("failed: Device didn't start correctly"));
    CHECK(f.host->recoveryAttempts() == 1);
    REQUIRE(f.dev() != nullptr);
    CHECK(f.type->created == 2); // a fresh device/driver instance
    CHECK(f.host->status().bufferSize == 128);
    CHECK(f.engine.maxBlock() == 128);
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    CHECK(f.logged("audio is running again"));
    CHECK(f.logFile().find("start_failed") != std::string::npos);
    CHECK(f.logFile().find("recovered") != std::string::npos);
    CHECK(f.savedAudio()["bufferSize"] == 128);
    CHECK(rt::violationCount() == v0);
}

TEST_CASE("audio host: watchdog reopens a device that stops calling back", "[audio]") {
    HostFixture f;
    REQUIRE(f.start().empty());
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    f.driver.preferred = 512; // the driver's panel changed meanwhile: the reopen must use it
    f.dev()->stall();
    f.tick(0); // absorb callbacks that ran before the stall
    f.tick(300); // 300 ms without callbacks: still tolerated
    CHECK(f.host->recoveryAttempts() == 0);
    f.tick(300); // 600 ms > 500 ms: stall logged, but JUCE's own reset (500 ms + reopen) gets its chance
    CHECK(f.host->recoveryAttempts() == 0);
    CHECK(f.logFile().find("[warn] stall") != std::string::npos);
    f.tick(1000); // 1.6 s > 1.5 s: reopen
    CHECK(f.host->recoveryAttempts() == 1);
    CHECK(f.logged("no audio callbacks for"));
    REQUIRE(f.dev() != nullptr);
    CHECK(f.host->status().bufferSize == 512);
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    CHECK(f.logged("audio is running again"));
    // Healthy again: no further reopen.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(f.waitCallbacks(2));
        f.tick(400);
    }
    CHECK(f.host->recoveryAttempts() == 1);
}

TEST_CASE("audio host: a pause during JUCE's own driver reset is left alone", "[audio]") {
    HostFixture f;
    REQUIRE(f.start().empty());
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    f.dev()->stall(); // the driver stops calling back while it reconfigures ...
    f.tick(0);
    f.tick(600);
    f.driver.preferred = 128;
    f.dev()->driverReset(false); // ... and JUCE's 500 ms reset timer reopens it
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    f.tick(1000);
    CHECK(f.host->recoveryAttempts() == 0);
    CHECK(f.type->created == 1); // never reopened by us
    CHECK(f.host->status().bufferSize == 128);
    CHECK(f.logged("buffer 256 -> 128"));
}

TEST_CASE("audio host: restart_audio failure is retried by the watchdog; set_audio_device failure is not", "[audio]") {
    HostFixture f;
    REQUIRE(f.start().empty());
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    f.driver.failOpens = 1;
    CHECK_FALSE(f.host->restart().empty());
    CHECK(f.dev() == nullptr);
    f.tick(500);
    CHECK(f.host->recoveryAttempts() == 0); // backoff
    f.tick(600);
    CHECK(f.host->recoveryAttempts() == 1);
    REQUIRE(f.dev() != nullptr);
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    CHECK(f.logged("audio is running again"));

    // The user asks for a device that does not exist: error in the reply, no automatic retries ...
    CHECK_FALSE(f.host->setDevice("ASIO", "Nope", 0, 0).empty());
    f.tick(2000);
    f.tick(2000);
    CHECK(f.host->recoveryAttempts() == 1);
    // ... and "Restart audio" brings back the last working device.
    REQUIRE(f.host->restart().empty());
    CHECK(f.host->status().name == kDeviceName.toStdString());
    REQUIRE(f.waitCallbacks());
}

TEST_CASE("audio host: a device that errors and stops is recovered", "[audio]") {
    // JUCE's AudioDeviceManager drops audioDeviceError (CallbackMaxSizeEnforcer does not forward it), so the
    // watchdog must catch the stopped callbacks.
    HostFixture f;
    REQUIRE(f.start().empty());
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    f.dev()->fail("USB device removed");
    uint64_t last = f.host->callbackCount();
    for (int stable = 0; stable < 20;) { // wait until the device thread stopped
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const uint64_t c = f.host->callbackCount();
        stable = c == last ? stable + 1 : 0;
        last = c;
    }
    f.tick(0);
    f.tick(1600);
    CHECK(f.host->recoveryAttempts() == 1);
    REQUIRE(f.waitCallbacks());
    f.tick(50);
    CHECK(f.host->status().running);
    CHECK(f.logged("audio is running again"));
}

TEST_CASE("audio host: failed reopens are retried with backoff until the driver is back", "[audio]") {
    HostFixture f;
    REQUIRE(f.start().empty());
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    f.driver.failOpens = 2;
    f.dev()->stall();
    f.tick(0);
    f.tick(1600);
    CHECK(f.host->recoveryAttempts() == 1); // failed: device closed
    CHECK(f.dev() == nullptr);
    f.tick(500);
    CHECK(f.host->recoveryAttempts() == 1); // backoff (1 s)
    f.tick(600);
    CHECK(f.host->recoveryAttempts() == 2); // failed again, next in 2 s
    f.tick(1500);
    CHECK(f.host->recoveryAttempts() == 2);
    f.tick(600);
    CHECK(f.host->recoveryAttempts() == 3);
    REQUIRE(f.dev() != nullptr);
    REQUIRE(f.waitCallbacks());
    f.tick(100);
    CHECK(f.logged("audio is running again"));
    CHECK(f.host->status().running);
}

TEST_CASE("audio host: a stale saved ASIO buffer is not forced; an explicit choice is kept", "[audio]") {
    HostFixture f;
    {
        std::ofstream s(f.root / "userdata" / "settings.json", std::ios::binary);
        s << R"({"audio":{"type":"ASIO","name":"Fake Steinberg UR22C","sampleRate":48000,"bufferSize":512}})";
    }
    f.driver.preferred = 128; // changed in the driver panel while keysynth was not running
    REQUIRE(f.start().empty());
    CHECK(f.host->status().bufferSize == 128);
    CHECK(f.savedAudio()["bufferExplicit"] == false);

    // The user picks 512 (the driver offers it): explicit, re-requested on the next start.
    REQUIRE(f.host->setDevice("ASIO", kDeviceName.toStdString(), 0, 512).empty());
    CHECK(f.host->status().bufferSize == 512);
    CHECK(f.savedAudio()["bufferExplicit"] == true);
    f.makeHost();
    REQUIRE(f.start().empty());
    CHECK(f.host->status().bufferSize == 512);
    REQUIRE(f.host->restart().empty()); // an explicit, still-offered size survives a restart
    CHECK(f.host->status().bufferSize == 512);
    f.tick(100);

    // The driver changes it behind our back: the driver wins and the explicit flag is dropped.
    f.driver.preferred = 256;
    f.dev()->driverReset(false);
    f.tick(100);
    CHECK(f.host->status().bufferSize == 256);
    CHECK(f.savedAudio()["bufferSize"] == 256);
    CHECK(f.savedAudio()["bufferExplicit"] == false);

    // restart(): the driver's current size (ASIO), never a stale one.
    f.driver.preferred = 64;
    REQUIRE(f.host->restart().empty());
    CHECK(f.host->status().bufferSize == 64);
    CHECK(f.engine.maxBlock() == 64);
    REQUIRE(f.waitCallbacks());
}

TEST_CASE("audio host: Yamaha-like driver offering one size", "[audio]") {
    HostFixture f;
    f.driver.onlyPreferred = true;
    REQUIRE(f.start({512, std::nullopt, ""}).empty()); // --asio-buffer 512: not offered
    CHECK(f.host->status().bufferSize == 256);
    CHECK(f.savedAudio()["bufferExplicit"] == false);
    f.driver.preferred = 1024;
    f.dev()->driverReset(false);
    CHECK(f.engine.maxBlock() == 1024);
    REQUIRE(f.waitCallbacks());
}

TEST_CASE("engine: device callbacks larger than maxBlock are split without allocation", "[audio][engine]") {
    Engine e;
    e.prepare(48000.0, 64);
    e.publish(std::make_unique<RackGraph>(48000.0, 64));
    std::vector<float> l(20000, 1.0f), r(20000, 1.0f), z(20000, 1.0f);
    float* outs[3] = {l.data(), r.data(), z.data()};
    const uint64_t v0 = rt::violationCount();
    const uint64_t b0 = e.telemetry().blocks.load();
    e.process(outs, 3, 1000);
    e.process(outs, 3, 20000); // > kMaxDeviceBlock too
    e.process(outs, 3, 7);
    CHECK(e.telemetry().blocks.load() - b0 == (1000 + 63) / 64 + (20000 + 63) / 64 + 1);
    CHECK(rt::violationCount() == v0);
    for (int i = 0; i < 20000; ++i) REQUIRE(z[static_cast<size_t>(i)] == 0.0f); // extra channels zeroed
    CHECK(l[19999] == 0.0f); // silence rendered all the way to the end
}
