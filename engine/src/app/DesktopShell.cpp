#include "app/DesktopShell.h"

#include "core/AppPaths.h"
#include "platform/DesktopIntegration.h"

#include <juce_events/juce_events.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <system_error>

namespace ks::app {

void setupGuiOutput(const std::filesystem::path& logsDir, bool rotate) {
#if defined(KS_GUI_SUBSYSTEM) && defined(_WIN32)
    // Redirected by the parent (pipes / files): the CRT already uses those handles.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    FILE* f = nullptr;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(logsDir, ec);
    const auto log = logsDir / "engine.log";
    if (rotate) std::filesystem::rename(log, logsDir / "engine.prev.log", ec);
    // _wfreopen (not _s): shared read/write, so the log can be read while the app runs. Both streams append (the
    // old log was renamed away), so stdout and stderr lines never overwrite each other.
    if (_wfreopen(log.c_str(), L"a", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IONBF, 0); // MSVC has no line buffering; crash-safe log
        if (_wfreopen(log.c_str(), L"a", stderr) != nullptr) setvbuf(stderr, nullptr, _IONBF, 0);
    }
#elif defined(KS_GUI_SUBSYSTEM) && defined(__APPLE__)
    // Started from Finder / login item / a link: stdout and stderr go nowhere (no terminal). From a terminal they
    // stay there, like an attached console on Windows.
    if (isatty(STDOUT_FILENO) || isatty(STDERR_FILENO)) return;
    std::error_code ec;
    std::filesystem::create_directories(logsDir, ec);
    const auto log = logsDir / "engine.log";
    if (rotate) std::filesystem::rename(log, logsDir / "engine.prev.log", ec);
    if (std::freopen(log.c_str(), "a", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        if (std::freopen(log.c_str(), "a", stderr) != nullptr) setvbuf(stderr, nullptr, _IONBF, 0);
    }
#else
    (void)logsDir;
    (void)rotate;
#endif
}

void reportFatal(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
#if defined(KS_GUI_SUBSYSTEM) && defined(_WIN32)
    const juce::String text = juce::String::fromUTF8(message.c_str());
    MessageBoxW(nullptr, text.toWideCharPointer(), L"keysynth", MB_OK | MB_ICONERROR);
#elif defined(KS_GUI_SUBSYSTEM) && defined(__APPLE__)
    // Blocking alert without a run loop (fatal startup errors exit right after).
    CFStringRef text = CFStringCreateWithCString(nullptr, message.c_str(), kCFStringEncodingUTF8);
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr, CFSTR("keysynth"),
                                   text ? text : CFSTR("keysynth failed to start"), nullptr, nullptr, nullptr,
                                   &response);
    if (text) CFRelease(text);
#endif
}

UiOpener::~UiOpener() {
    if (worker_.joinable()) worker_.join();
}

void UiOpener::openPreferred() {
    if (busy_.exchange(true)) return;
    if (worker_.joinable()) worker_.join(); // the previous probe has finished (busy_ was false)
    worker_ = std::thread([this, local = localAvailable_ && localAvailable_() ? localUrl_ : std::string()] {
        const bool web = platform::probeHealth(platform::kWebHealthUrl, 3000);
        const std::string url = web || local.empty() ? std::string(platform::kWebUiUrl) : local;
        std::printf("open UI: %s%s\n", url.c_str(), web ? "" : " (mvv42.ru unreachable)");
        std::fflush(stdout);
        // ShellExecute on the message thread (COM-initialised). Captures only the URL: safe after ~UiOpener.
        juce::MessageManager::callAsync([url] { platform::openInBrowser(url); });
        busy_.store(false);
    });
}

void UiOpener::openLocal() const {
    if (localAvailable_ && localAvailable_()) platform::openInBrowser(localUrl_);
}

} // namespace ks::app
