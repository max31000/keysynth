#pragma once
// Tray icon of the desktop app (ARCHITECTURE §16). Message thread only. Pure UI: every action is a callback wired
// by main.cpp, so the tray never touches engine state itself.

#include "platform/MenuBar.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>

namespace ks::app {

struct TrayActions {
    std::function<std::string()> tooltip; // status text, refreshed every 2 s
    std::function<void()> openUi;         // web UI (local fallback when offline)
    std::function<void()> openLocal;      // local UI (http://127.0.0.1:<http-port>/)
    std::function<bool()> localAvailable; // the static UI server is running
    std::function<std::string()> restartAudio; // "" = ok, else an error shown in a balloon
    std::function<bool()> autostartEnabled;
    std::function<bool(bool)> setAutostart; // false = registry write failed
    std::function<void()> quit;
    std::function<void()> poll; // called at 10 Hz (cross-process commands)
    // macOS widget only (platform/MenuBar): live status snapshot and "show the log".
    std::function<platform::MenuBarStatus()> status;
    std::function<void()> openLogs;
};

#if defined(__APPLE__)
// macOS: a native menu-bar item with a status popover (platform/MenuBar); JUCE's macOS tray has no working menu,
// tooltip or balloons.
class Tray final : private juce::Timer {
public:
    Tray(TrayActions actions, std::string version);
    ~Tray() override;

    void notify(const std::string& title, const std::string& text);

private:
    void timerCallback() override;

    TrayActions a_;
    std::unique_ptr<platform::MenuBar> bar_;
    int ticks_ = 0;
};
#else
class Tray final : public juce::SystemTrayIconComponent, private juce::Timer {
public:
    Tray(TrayActions actions, std::string version);
    ~Tray() override;

    void notify(const std::string& title, const std::string& text);

private:
    void mouseDown(const juce::MouseEvent& e) override;
    void timerCallback() override;
    void showMenu();
    void refreshTooltip();

    TrayActions a_;
    std::string version_;
    std::string lastTooltip_;
    int ticks_ = 0;
};
#endif

// Menu/tooltip language: Russian when the Windows display language is Russian, else English.
bool trayRussian();

} // namespace ks::app
