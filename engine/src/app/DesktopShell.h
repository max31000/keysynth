#pragma once
// Desktop-app glue for main.cpp (ARCHITECTURE §16): console/log setup of the windows-subsystem build, fatal error
// reporting, and the asynchronous "open the UI" action (web UI if mvv42.ru is reachable, else the local UI).

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace ks::app {

// keysynth-app (KS_GUI_SUBSYSTEM) has no console: attach to the parent's console when started from one, else
// append stdout/stderr to <logsDir>/engine.log; `rotate` (primary desktop instance only) first keeps the previous
// run as engine.prev.log. No-op in the console build.
void setupGuiOutput(const std::filesystem::path& logsDir, bool rotate);

// stderr, plus a message box in the windows-subsystem build (nobody sees stderr there).
void reportFatal(const std::string& message);

class UiOpener {
public:
    UiOpener(std::string localUrl, bool localAvailable) : localUrl_(std::move(localUrl)), localAvailable_(localAvailable) {}
    ~UiOpener();
    UiOpener(const UiOpener&) = delete;
    UiOpener& operator=(const UiOpener&) = delete;

    // Message thread. Probes the web UI's health URL on a worker (<= 3 s), then opens the web UI or, when it is
    // unreachable and the local UI is served, the local one. Ignored while a probe is running.
    void openPreferred();
    void openLocal() const;
    const std::string& localUrl() const noexcept { return localUrl_; }

private:
    std::string localUrl_;
    bool localAvailable_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
};

} // namespace ks::app
