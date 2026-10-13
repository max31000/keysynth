#include "app/Tray.h"

#include <cstddef>

namespace ks::app {

extern const unsigned char kTrayIconPng[]; // generated from assets/icon/keysynth-32.png (engine/CMakeLists.txt)
extern const std::size_t kTrayIconPngSize;
extern const unsigned char kTrayTemplatePng[]; // assets/icon/keysynth-tray-template.png (macOS menu bar)
extern const std::size_t kTrayTemplatePngSize;
extern const unsigned char kAppIcon64Png[]; // assets/icon/keysynth-64.png (macOS widget header)
extern const std::size_t kAppIcon64PngSize;

namespace {
[[maybe_unused]] juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
[[maybe_unused]] juce::String tr(const char* ru, const char* en) { return trayRussian() ? u8(ru) : u8(en); }
} // namespace

bool trayRussian() { // display language (Windows UI language / macOS preferred language)
    static const bool ru = juce::SystemStats::getDisplayLanguage().startsWithIgnoreCase("ru");
    return ru;
}

#if defined(__APPLE__)

Tray::Tray(TrayActions actions, std::string version) : a_(std::move(actions)) {
    platform::MenuBarActions m;
    m.status = [this, version] {
        platform::MenuBarStatus s = a_.status ? a_.status() : platform::MenuBarStatus{};
        if (s.version.empty()) s.version = version;
        return s;
    };
    m.tooltip = a_.tooltip;
    m.openUi = a_.openUi;
    m.openLocal = a_.openLocal;
    m.restartAudio = a_.restartAudio;
    m.setAutostart = a_.setAutostart;
    m.openLogs = a_.openLogs;
    m.quit = a_.quit;
    bar_ = std::make_unique<platform::MenuBar>(std::move(m), trayRussian(), kTrayTemplatePng, kTrayTemplatePngSize,
                                               kAppIcon64Png, kAppIcon64PngSize);
    startTimerHz(10);
}

Tray::~Tray() { stopTimer(); }

void Tray::notify(const std::string& title, const std::string& text) { bar_->notify(title, text); }

void Tray::timerCallback() {
    if (a_.poll) a_.poll();
    if (++ticks_ % 20 == 0) bar_->refresh();
}

#else

Tray::Tray(TrayActions actions, std::string version) : a_(std::move(actions)), version_(std::move(version)) {
    const juce::Image icon = juce::ImageFileFormat::loadFrom(kTrayIconPng, kTrayIconPngSize);
    setIconImage(icon, icon);
    refreshTooltip();
    startTimerHz(10);
}

Tray::~Tray() { stopTimer(); }

void Tray::notify(const std::string& title, const std::string& text) {
    showInfoBubble(u8(title.c_str()), u8(text.c_str()));
}

void Tray::mouseDown(const juce::MouseEvent& e) {
    // Left click opens the UI (like other tray apps); right click shows the menu.
    if (e.mods.isPopupMenu()) showMenu();
    else if (a_.openUi) a_.openUi();
}

void Tray::showMenu() {
    juce::PopupMenu m;
    m.addSectionHeader("keysynth " + u8(version_.c_str()));
    m.addItem(tr("Открыть keysynth", "Open keysynth"), [this] { if (a_.openUi) a_.openUi(); });
    m.addItem(tr("Открыть локально", "Open locally"), a_.localAvailable && a_.localAvailable(),
              false, [this] { if (a_.openLocal) a_.openLocal(); });
    m.addSeparator();
    m.addItem(tr("Перезапустить звук", "Restart audio"), [this] {
        if (!a_.restartAudio) return;
        const std::string err = a_.restartAudio();
        if (!err.empty())
            notify(trayRussian() ? "Звук не перезапущен" : "Audio restart failed", err);
        refreshTooltip();
    });
    const bool autostart = a_.autostartEnabled && a_.autostartEnabled();
    m.addItem(tr("Автозапуск", "Start with Windows"), true, autostart, [this, autostart] {
        if (a_.setAutostart && !a_.setAutostart(!autostart))
            notify("keysynth", trayRussian() ? "Не удалось изменить автозапуск" : "Could not change autostart");
    });
    m.addSeparator();
    m.addItem(tr("Выход", "Quit"), [this] { if (a_.quit) a_.quit(); });
    // Foreground first, or the menu does not close when the user clicks elsewhere.
    juce::Process::makeForegroundProcess();
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void Tray::timerCallback() {
    if (a_.poll) a_.poll();
    if (++ticks_ % 20 == 0) refreshTooltip();
}

void Tray::refreshTooltip() {
    if (!a_.tooltip) return;
    std::string t = a_.tooltip();
    if (t == lastTooltip_) return;
    lastTooltip_ = t;
    setIconTooltip(u8(t.c_str())); // Windows truncates at 127 UTF-16 units
}

#endif

} // namespace ks::app
