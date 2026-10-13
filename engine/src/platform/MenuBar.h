#pragma once
// macOS menu-bar item of the desktop app (ARCHITECTURE §16 *macOS*): an NSStatusItem with a popover status widget
// (left click) and a native menu (right click / control-click). Implemented in MenuBar_mac.mm; on other platforms
// the class is not available (the Windows tray stays JUCE's SystemTrayIconComponent, app/Tray). Message (= main)
// thread only. Pure UI: data comes from `status()`, every action is a callback wired by app/.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ks::platform {

// Snapshot for the widget, polled ~4 Hz while the popover is open (and every 2 s for the tooltip).
struct MenuBarStatus {
    std::string version;
    // Engine / control port: ok = the WS port is ours; `portProblem` = why not (user-facing, localized).
    bool engineOk = true;
    std::string portProblem;
    int wsPort = 7341;
    int localClients = 0;  // local UI sockets
    int remoteClients = 0; // relay controllers
    // Audio
    bool audioRunning = false;
    std::string audioType, audioDevice;
    double sampleRate = 0.0;
    int bufferSize = 0;
    double outputLatencyMs = 0.0;
    float cpu = 0.0f;           // 0..1 (peak of the last poll window)
    int voices = 0;
    float masterDb = -120.0f;   // peak of the louder channel
    std::string preset;         // patch name
    std::vector<std::string> midiInputs;
    // Cloud
    bool cloudEnabled = false, cloudLinked = false;
    std::string cloudAccount;
    // Actions' state
    bool localAvailable = false;
    bool autostart = false;
};

struct MenuBarActions {
    std::function<MenuBarStatus()> status;
    std::function<std::string()> tooltip;
    std::function<void()> openUi;
    std::function<void()> openLocal;
    std::function<std::string()> restartAudio; // "" = ok, else the error shown in the widget
    std::function<bool(bool)> setAutostart;    // false = failed
    std::function<void()> openLogs;
    std::function<void()> quit;
};

class MenuBar {
public:
    // `templatePng` = the menu-bar glyph (black on transparent; tinted by macOS), `iconPng` = the colour app icon
    // shown in the widget header. `russian` picks the UI language.
    MenuBar(MenuBarActions actions, bool russian, const void* templatePng, size_t templateSize, const void* iconPng,
            size_t iconSize);
    ~MenuBar();
    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    // A notice (port busy, audio restart failed, ...): shown as a banner in the widget until replaced or cleared;
    // the menu-bar icon shows an attention dot meanwhile. Empty text clears it.
    void notify(const std::string& title, const std::string& text);
    // Re-reads the tooltip (call every few seconds).
    void refresh();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ks::platform
