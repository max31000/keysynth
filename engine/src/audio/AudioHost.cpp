#include "audio/AudioHost.h"

#include "core/RtCheck.h"
#include "platform/ThreadPriority.h"

#include <algorithm>
#include <cstdio>
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
            {"running", s.running},
            {"hasControlPanel", s.hasControlPanel},
            {"panelOpen", s.panelOpen}};
}

namespace {
std::string str(const juce::String& s) { return s.toStdString(); }
juce::String jstr(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

bool preferredAsioName(const juce::String& n) {
    return n.containsIgnoreCase("Steinberg") || n.containsIgnoreCase("Yamaha");
}

// ASIO drivers own their buffer size (control panel) and may change it (and the rate) at any time: reopen with
// the driver's current values instead of re-requesting ours (ARCHITECTURE §6).
bool followsDriver(const std::string& type) { return type == "ASIO"; }

constexpr std::uintmax_t kMaxLogBytes = 1u << 20;

std::string fmtMs(double ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", ms);
    return b;
}

// Marks a device restart we initiated ourselves (so audioDeviceAboutToStart can tell it from a driver restart).
struct SelfOp {
    int& depth;
    SelfOp(int& d, std::string& reasonSlot, const std::string& reason) : depth(d) {
        ++depth;
        reasonSlot = reason;
    }
    ~SelfOp() { --depth; }
    SelfOp(const SelfOp&) = delete;
    SelfOp& operator=(const SelfOp&) = delete;
};
} // namespace

AudioHost::AudioHost(Engine& engine, AppPaths paths, std::function<void()> onPrepared)
    : engine_(engine), paths_(std::move(paths)), onPrepared_(std::move(onPrepared)) {}

AudioHost::~AudioHost() {
    cancelPendingUpdate();
    stop();
}

void AudioHost::addDeviceType(std::unique_ptr<juce::AudioIODeviceType> type) { dm_.addAudioDeviceType(std::move(type)); }

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
    if (s.type.empty() || s.type == "None" || s.name.empty()) return;
    j["audio"] = {{"type", s.type},
                  {"name", s.name},
                  {"sampleRate", s.sampleRate},
                  {"bufferSize", s.bufferSize},
                  // ASIO: re-request bufferSize at startup only when the user chose it (else: the driver's size).
                  {"bufferExplicit", explicitBuffer_ > 0 && explicitBuffer_ == s.bufferSize}};
    std::error_code ec;
    std::filesystem::create_directories(paths_.userdata, ec);
    std::ofstream f(paths_.settingsFile, std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(2) << '\n';
}

void AudioHost::logEvent(const std::string& level, const std::string& event, const std::string& detail, bool notify,
                         juce::AudioIODevice* dev) {
    if (dev == nullptr) dev = dm_.getCurrentAudioDevice();
    std::string devText;
    if (dev != nullptr) {
        const double sr = dev->getCurrentSampleRate();
        const int bs = dev->getCurrentBufferSizeSamples();
        char b[160];
        std::snprintf(b, sizeof b, " sr=%.0f buffer=%d in=%.2fms out=%.2fms", sr, bs,
                      sr > 0 ? 1000.0 * dev->getInputLatencyInSamples() / sr : 0.0,
                      sr > 0 ? 1000.0 * dev->getOutputLatencyInSamples() / sr : 0.0);
        devText = " type=" + str(dev->getTypeName()) + " name=\"" + str(dev->getName()) + "\"" + b;
    } else {
        devText = " type=" + str(dm_.getCurrentAudioDeviceType()) + " (no device)";
    }
    const std::string line = str(juce::Time::getCurrentTime().toISO8601(true)) + " [" + level + "] " + event +
                             devText + (detail.empty() ? "" : " | " + detail);
    {
        MutexLock lock(logMutex_);
        const auto file = logFile();
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > kMaxLogBytes) {
            auto old = file;
            old += ".1";
            std::filesystem::remove(old, ec);
            std::filesystem::rename(file, old, ec);
        }
        std::ofstream f(file, std::ios::binary | std::ios::app);
        if (f) f << line << '\n';
    }
    if (notify) {
        std::string msg = "audio: " + detail;
        if (dev != nullptr) {
            char b[96];
            std::snprintf(b, sizeof b, " (%.0f Hz, buffer %d)", dev->getCurrentSampleRate(),
                          dev->getCurrentBufferSizeSamples());
            msg += " — " + str(dev->getName()) + b;
        }
        MutexLock lock(notesMutex_);
        notes_.push_back({level, std::move(msg), true});
    }
}

void AudioHost::flushNotes() {
    std::vector<Note> n;
    {
        MutexLock lock(notesMutex_);
        n.swap(notes_);
    }
    for (const Note& x : n) {
        if (onLog) onLog(x.level, x.msg, x.notify);
        else std::fprintf(stderr, "[%s] %s\n", x.level.c_str(), x.msg.c_str());
    }
}

void AudioHost::rememberTarget() {
    if (auto* dev = dm_.getCurrentAudioDevice()) {
        target_.type = str(dev->getTypeName());
        target_.name = str(dev->getName());
    }
}

std::string AudioHost::start(const Options& opt, bool startTimerNow) {
    dm_.addAudioCallback(this);
    const auto& types = dm_.getAvailableDeviceTypes(); // creates + scans device types (unless addDeviceType)

    // 1) saved settings, 2) ASIO Steinberg/Yamaha, 3) system default.
    std::string type, name;
    double sr = 0.0;
    int buffer = 0;
    bool savedExplicit = false;
    const nlohmann::json settings = loadSettings();
    if (auto a = settings.find("audio"); a != settings.end() && a->is_object()) {
        type = a->value("type", std::string());
        name = a->value("name", std::string());
        sr = a->value("sampleRate", 0.0);
        buffer = a->value("bufferSize", 0);
        savedExplicit = a->value("bufferExplicit", false);
    }
    // A saved ASIO size the user did not choose is the driver's old panel value: never force it (the panel may
    // have changed since). 0 = the driver's preferred size.
    if (followsDriver(type) && !savedExplicit) buffer = 0;
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
    {
        SelfOp op(selfOps_, selfReason_, "startup");
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
    }
    auto* dev = dm_.getCurrentAudioDevice();
    if (dev == nullptr && err.isEmpty()) err = "no audio device could be opened";
    if (dev == nullptr && !type.empty() && !name.empty()) {
        // Unplugged / busy at launch: keep trying (watchdog, backoff) and let `restart_audio` open it later.
        target_ = {type, name};
        explicitBuffer_ = followsDriver(type) && !savedExplicit && !opt.bufferSize ? 0 : buffer;
        expectRunning_ = true;
    }
    if (dev != nullptr) {
        rememberTarget();
        // Explicit = re-requested later. ASIO: only when the driver offers a choice and accepted ours.
        const bool choice = !followsDriver(target_.type) || dev->getAvailableBufferSizes().size() > 1;
        explicitBuffer_ = buffer > 0 && choice && dev->getCurrentBufferSizeSamples() == buffer ? buffer : 0;
        expectRunning_ = true;
        saveSettings();
    }
    logEvent(err.isEmpty() ? "info" : "error", "open", err.isEmpty() ? "startup" : "startup failed: " + str(err), false);
    flushNotes();
    if (startTimerNow) startTimer(100);
    return str(err);
}

void AudioHost::stop() {
    stopTimer();
    expectRunning_ = false;
    {
        SelfOp op(selfOps_, selfReason_, "shutdown");
        dm_.removeAudioCallback(this);
        dm_.closeAudioDevice();
    }
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
    s.hasControlPanel = dev->hasControlPanel();
    s.panelOpen = panelOpen_.load();
    return s;
}

std::string AudioHost::openControlPanel() {
    if (panelOpen_.load()) return "the driver's control panel is already open";
    auto* dev = dm_.getCurrentAudioDevice();
    if (dev == nullptr) return "no audio device is open";
    if (!dev->hasControlPanel()) return "the " + str(dev->getTypeName()) + " device has no control panel";
    // Deferred: the reply goes out first, and a modal driver panel then runs its own loop on the message thread.
    panelRequested_.store(true);
    triggerAsyncUpdate();
    return {};
}

void AudioHost::showControlPanelNow() {
    if (panelOpen_.load()) return;
    auto* dev = dm_.getCurrentAudioDevice();
    if (dev == nullptr || !dev->hasControlPanel()) return;
    // A modal panel runs a nested message loop: protocol requests keep being handled meanwhile. panelOpen_ makes
    // openControlPanel/setDevice/restart refuse (they could delete `dev` while its showControlPanel is on the stack),
    // listDevices skip the rescan and the watchdog pause; clients see status().panelOpen and disable the controls.
    struct Reset {
        std::atomic<bool>& f;
        ~Reset() { f.store(false); }
    } reset{panelOpen_};
    panelOpen_.store(true);
    if (onChanged) onChanged();
    logEvent("info", "panel_open", "", false, dev);
    // JUCE ASIO: true when the panel was modal (> 300 ms) — settings may have changed, reopen with the driver's
    // (new) preferred buffer size. Non-modal panels (the Yamaha Steinberg driver opens a separate settings app)
    // return at once; a later buffer change there makes the driver send a reset request, JUCE reopens the device
    // and audioDeviceAboutToStart re-reports it (or the watchdog recovers it when that reopen failed).
    const bool modal = dev->showControlPanel();
    logEvent("info", "panel_closed", modal ? "modal" : "non-modal (driver app)", false);
    // Only reopen the device the panel belonged to (a driver reset may have replaced it meanwhile).
    if (modal && dm_.getCurrentAudioDevice() == dev) {
        const std::string err = reopen("control panel closed");
        if (!err.empty()) {
            logEvent("error", "reopen_failed", "reopening the device after the control panel failed: " + err, true);
            recoverNow_ = true; // the watchdog keeps trying
            recoverReason_ = "reopen after the control panel failed";
        }
    }
    changed_.store(true); // re-reported (panelOpen false again) by handleAsyncUpdate after this returns
}

void AudioHost::handleAsyncUpdate() {
    if (panelRequested_.exchange(false)) showControlPanelNow();
    if (changed_.exchange(false)) {
        if (dm_.getCurrentAudioDevice() != nullptr) saveSettings();
        if (onChanged) onChanged();
    }
    flushNotes();
}

void AudioHost::timerCallback() { tick(juce::Time::getMillisecondCounterHiRes()); }

void AudioHost::tick(double nowMs) {
    lastTickMs_ = nowMs;
    if (changed_.exchange(false)) {
        if (dm_.getCurrentAudioDevice() != nullptr) saveSettings();
        if (onChanged) onChanged();
    }
    if (errorPending_.load(std::memory_order_acquire)) {
        const std::string msg(errorText_.data());
        errorPending_.store(false);
        errorWriting_.store(false, std::memory_order_release);
        logEvent("error", "device_error", "the driver reported an error: " + msg, true);
        recoverNow_ = true;
        recoverReason_ = "device error: " + msg;
    }
    if (stoppedOffThread_.exchange(false)) logEvent("warn", "stopped", "stopped from the device thread", false);

    const uint64_t c = callbacks_.load(std::memory_order_relaxed);
    if (startEpoch_ != seenEpoch_) { // (re)started: new baseline
        seenEpoch_ = startEpoch_;
        lastCount_ = startCount_; // callbacks since the start count as progress

        lastProgressMs_ = nowMs;
    }
    if (c != lastCount_) {
        lastCount_ = c;
        lastProgressMs_ = nowMs;
        if (stallLogged_) {
            stallLogged_ = false;
            if (!recovering_) logEvent("info", "stall_end", "callbacks resumed by themselves", false);
        }
        if (recovering_) {
            recovering_ = false;
            retryDelayMs_ = 0.0;
            nextRetryMs_ = 0.0;
            logEvent("info", "recovered", "audio is running again", true);
        }
    }
    if (panelOpen_.load()) { // modal panel: the driver may legitimately pause; never reopen under it
        lastProgressMs_ = nowMs;
        flushNotes();
        return;
    }
    std::string reason;
    if (expectRunning_) {
        if (recoverNow_) reason = recoverReason_;
        else if (dm_.getCurrentAudioDevice() == nullptr) reason = "the device was closed";
        else if (const double quiet = nowMs - lastProgressMs_; quiet > watchdog_.stallMs) {
            if (!stallLogged_) { // file only: JUCE's own reset (500 ms) may still bring it back
                stallLogged_ = true;
                logEvent("warn", "stall", "no audio callbacks for " + fmtMs(quiet) + " ms", false);
            }
            if (quiet > watchdog_.recoverMs) reason = "no audio callbacks for " + fmtMs(quiet) + " ms";
        }
    }
    if (!reason.empty() && nowMs >= nextRetryMs_) {
        recoverNow_ = false;
        const bool first = !recovering_;
        ++recoveryAttempts_;
        logEvent(first ? "error" : "warn", "recover",
                 reason + " — reopening with the driver's current settings (attempt " +
                     std::to_string(recoveryAttempts_) + ")",
                 first);
        recovering_ = true;
        stallLogged_ = false;
        const std::string err = reopen("recovery: " + reason);
        retryDelayMs_ = retryDelayMs_ <= 0.0 ? watchdog_.firstRetryMs : std::min(retryDelayMs_ * 2.0, watchdog_.maxRetryMs);
        nextRetryMs_ = nowMs + retryDelayMs_;
        lastProgressMs_ = nowMs; // grace period for the reopened device
        if (!err.empty())
            logEvent("error", "recover_failed", "reopening failed: " + err + "; retrying in " +
                                                     fmtMs(retryDelayMs_ / 1000.0) + " s", first);
    }
    flushNotes();
}

std::string AudioHost::reopen(const std::string& reason) {
    if (target_.type.empty() || target_.name.empty()) return "no audio device to restart";
    SelfOp op(selfOps_, selfReason_, reason);
    if (auto* dev = dm_.getCurrentAudioDevice()) {
        const int x = dev->getXRunCount();
        if (x > 0) xrunBase_.fetch_add(static_cast<uint64_t>(x));
    }
    if (str(dm_.getCurrentAudioDeviceType()) != target_.type) dm_.setCurrentAudioDeviceType(jstr(target_.type), true);
    auto setup = dm_.getAudioDeviceSetup();
    setup.outputDeviceName = jstr(target_.name);
    setup.inputDeviceName = {};
    // The user's explicit size if any (JUCE falls back to the driver's preferred size when it is not offered),
    // else 0 = the driver's preferred (ASIO: its control panel value), never a stale one.
    setup.bufferSize = explicitBuffer_;
    if (followsDriver(target_.type)) setup.sampleRate = 0; // the driver's current rate
    // A fresh device object = a fresh driver instance (JUCE ASIO reads the new preferred size on load).
    dm_.closeAudioDevice();
    // Callbacks of the closed device are not progress of the new one (else tick() would call a failed reopen
    // "recovered").
    lastCount_ = callbacks_.load(std::memory_order_relaxed);
    const juce::String err = dm_.setAudioDeviceSetup(setup, true);
    if (err.isEmpty() && dm_.getCurrentAudioDevice() != nullptr) {
        expectRunning_ = true;
        if (explicitBuffer_ > 0 && dm_.getCurrentAudioDevice()->getCurrentBufferSizeSamples() != explicitBuffer_)
            explicitBuffer_ = 0; // not accepted any more: the driver's size wins
        saveSettings();
        changed_.store(true);
        triggerAsyncUpdate();
        return {};
    }
    changed_.store(true);
    triggerAsyncUpdate();
    return err.isEmpty() ? "the device did not reopen" : str(err);
}

std::string AudioHost::restart() {
    if (panelOpen_.load()) return "close the driver's control panel first";
    logEvent("info", "restart", "requested (restart_audio)", false);
    const std::string err = reopen("restart_audio");
    if (!err.empty()) {
        logEvent("error", "restart_failed", err, false); // the reply carries the error
        if (!target_.type.empty()) {
            expectRunning_ = true; // the watchdog keeps retrying
            recovering_ = true;
            retryDelayMs_ = watchdog_.firstRetryMs;
            nextRetryMs_ = lastTickMs_ + retryDelayMs_;
        }
    } else {
        recoverNow_ = false;
        recovering_ = false;
        retryDelayMs_ = 0.0;
        nextRetryMs_ = 0.0;
    }
    // restart_audio reports the change itself (`devices` + `state`); drop the async re-report.
    changed_.store(false);
    flushNotes();
    return err;
}

AudioDeviceList AudioHost::listDevices() {
    AudioDeviceList l;
    for (auto* t : dm_.getAvailableDeviceTypes()) {
        l.types.push_back(str(t->getTypeName()));
        if (!panelOpen_.load()) t->scanForDevices(); // no driver probing under an open (modal) driver panel
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
    if (panelOpen_.load()) return "close the driver's control panel first";
    bool typeOk = false;
    for (auto* t : dm_.getAvailableDeviceTypes()) typeOk = typeOk || str(t->getTypeName()) == type;
    if (!typeOk) return "unknown device type '" + type + "'";
    juce::String err;
    {
        SelfOp op(selfOps_, selfReason_, "set_audio_device");
        if (auto* dev = dm_.getCurrentAudioDevice()) {
            const int x = dev->getXRunCount();
            if (x > 0) xrunBase_.fetch_add(static_cast<uint64_t>(x));
        }
        if (str(dm_.getCurrentAudioDeviceType()) != type) dm_.setCurrentAudioDeviceType(jstr(type), true);
        auto setup = dm_.getAudioDeviceSetup();
        setup.outputDeviceName = jstr(name);
        setup.inputDeviceName = {};
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.useDefaultOutputChannels = true;
        if (sampleRate > 0) setup.sampleRate = sampleRate;
        if (bufferSize > 0) setup.bufferSize = bufferSize;
        err = dm_.setAudioDeviceSetup(setup, true);
    }
    if (err.isEmpty() && dm_.getCurrentAudioDevice() != nullptr) {
        auto* dev = dm_.getCurrentAudioDevice();
        rememberTarget();
        const bool choice = !followsDriver(target_.type) || dev->getAvailableBufferSizes().size() > 1;
        explicitBuffer_ = bufferSize > 0 && choice && dev->getCurrentBufferSizeSamples() == bufferSize ? bufferSize : 0;
        expectRunning_ = true;
        recoverNow_ = recovering_ = false;
        retryDelayMs_ = 0.0;
        nextRetryMs_ = 0.0;
        saveSettings();
        logEvent("info", "set_device", "set_audio_device", false);
    } else {
        // The user sees the error in the reply; no automatic retries of a device they just asked for.
        expectRunning_ = false;
        logEvent("error", "set_device_failed", "set_audio_device " + type + " / " + name + ": " + str(err), false);
    }
    // The synchronous restart flagged an async re-report; set_audio_device reports the change itself (`devices`
    // to all + `state`), so drop it instead of sending everything twice.
    changed_.store(false);
    flushNotes();
    return str(err);
}

uint64_t AudioHost::xruns() const {
    auto* dev = dm_.getCurrentAudioDevice();
    const int x = dev ? dev->getXRunCount() : 0;
    return xrunBase_.load() + (x > 0 ? static_cast<uint64_t>(x) : 0) + engine_.telemetry().overloads.load();
}

void AudioHost::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    // Message thread (JUCE opens/restarts devices there — including the ASIO driver-reset timer); no callback runs.
    const bool self = selfOps_ > 0;
    const std::string reason = self ? selfReason_ : "driver restart (reset request / settings changed in the driver)";
    // JUCE's ASIO reset handler restarts callbacks even when its reopen failed (device closed, error set): such a
    // device reports "playing" but never calls back. Recover at the next tick (not here: we are inside its stack).
    if (!device->isOpen() || device->getLastError().isNotEmpty()) {
        running_.store(false);
        const std::string e = str(device->getLastError());
        logEvent("error", "start_failed", reason + " failed: " + (e.empty() ? "device not open" : e), true, device);
        if (!self) {
            recoverNow_ = true;
            recoverReason_ = "driver restart failed" + (e.empty() ? std::string() : ": " + e);
            expectRunning_ = true;
        }
        return;
    }
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    const int maxBlock = std::clamp(bs > 0 ? bs : 512, 1, Engine::kMaxDeviceBlock);
    // Re-prepare only when the format changed (or there is no graph yet); Engine::process splits any larger
    // callback, so a device that delivers more than it announced is still safe.
    if (sr != engine_.sampleRate() || maxBlock != engine_.maxBlock() || engine_.latestGraph() == nullptr) {
        engine_.prepare(sr, maxBlock);
        if (onPrepared_) onPrepared_();
    }
    promoted_.store(false); // the driver may use a new thread: register it with MMCSS again
    running_.store(true);
    startCount_ = callbacks_.load(std::memory_order_relaxed);
    ++startEpoch_;
    const bool changed = lastBs_ != 0 && (sr != lastSr_ || bs != lastBs_);
    std::string detail = reason;
    if (changed) {
        char b[128];
        std::snprintf(b, sizeof b, "; buffer %d -> %d, %.0f -> %.0f Hz", lastBs_, bs, lastSr_, sr);
        detail += b;
    }
    if (!self) {
        expectRunning_ = true;
        if (followsDriver(str(device->getTypeName())) && bs != explicitBuffer_) explicitBuffer_ = 0;
    }
    // Toast only for a driver restart that changed something (kAsioResyncRequest restarts with the same format).
    logEvent("info", "start", detail, !self && changed, device);
    if (self && changed) {
        // User/host-initiated change: plain info line (no toast).
        char b[200];
        std::snprintf(b, sizeof b, "audio: %s, %.0f Hz, buffer %d samples, output latency %.1f ms",
                      str(device->getName()).c_str(), sr, bs,
                      sr > 0 ? 1000.0 * device->getOutputLatencyInSamples() / sr : 0.0);
        MutexLock lock(notesMutex_);
        notes_.push_back({"info", b, false});
    }
    lastSr_ = sr;
    lastBs_ = bs;
    // Re-query/report the device asynchronously (buffer size may have been changed by the driver's own panel).
    changed_.store(true);
    triggerAsyncUpdate();
}

void AudioHost::audioDeviceStopped() {
    running_.store(false);
    if (juce::MessageManager::existsAndIsCurrentThread()) {
        if (selfOps_ == 0) logEvent("warn", "stopped", "stopped by the driver", false);
    } else {
        stoppedOffThread_.store(true);
    }
}

void AudioHost::audioDeviceError(const juce::String& message) {
    // NB: JUCE's AudioDeviceManager currently drops device errors (its CallbackMaxSizeEnforcer does not forward
    // audioDeviceError), so in practice a failing device is caught by the stall watchdog. Kept for direct callers.
    // Any thread (often the device thread): no IO/allocation here — copy into the fixed buffer, tick() logs it.
    running_.store(false);
    if (!errorWriting_.exchange(true, std::memory_order_acq_rel)) {
        message.copyToUTF8(errorText_.data(), errorText_.size());
        errorPending_.store(true, std::memory_order_release);
    }
}

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
    callbacks_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace ks
