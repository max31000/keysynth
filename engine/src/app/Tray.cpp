#include "app/Tray.h"

#include <cstddef>

namespace ks::app {

extern const unsigned char kTrayIconPng[]; // generated from assets/icon/keysynth-32.png (engine/CMakeLists.txt)
extern const std::size_t kTrayIconPngSize;
extern const unsigned char kTrayTemplatePng[]; // assets/icon/keysynth-tray-template.png (macOS menu bar)
extern const std::size_t kTrayTemplatePngSize;

namespace {
juce::String u8(const char* s) { return juce::String::fromUTF8(s); }
juce::String tr(const char* ru, const char* en) { return trayRussian() ? u8(ru) : u8(en); }
} // namespace

bool trayRussian() {
    static const bool ru = juce::SystemStats::getDisplayLanguage().startsWithIgnoreCase("ru");
    return ru;
}

Tray::Tray(TrayActions actions, std::string version) : a_(std::move(actions)), version_(std::move(version)) {
    const juce::Image icon = juce::ImageFileFormat::loadFrom(kTrayIconPng, kTrayIconPngSize);
    // macOS draws the template image (tinted for light/dark menu bars); Windows the colour one.
    const juce::Image tmpl = juce::ImageFileFormat::loadFrom(kTrayTemplatePng, kTrayTemplatePngSize);
    setIconImage(icon, tmpl);
    refreshTooltip();
    startTimerHz(10);
}

Tray::~Tray() { stopTimer(); }

void Tray::notify(const std::string& title, const std::string& text) {
    showInfoBubble(u8(title.c_str()), u8(text.c_str()));
}

void Tray::mouseDown(const juce::MouseEvent& e) {
#if defined(__APPLE__)
    // Menu bar convention: any click shows the menu ("Open keysynth" is its first item).
    (void)e;
    showMenu();
#else
    // Left click opens the UI (like other tray apps); right click shows the menu.
    if (e.mods.isPopupMenu()) showMenu();
    else if (a_.openUi) a_.openUi();
#endif
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
#if defined(__APPLE__)
    m.addItem(tr("Открывать при входе", "Open at Login"), true, autostart, [this, autostart] {
#else
    m.addItem(tr("Автозапуск", "Start with Windows"), true, autostart, [this, autostart] {
#endif
        if (a_.setAutostart && !a_.setAutostart(!autostart))
            notify("keysynth", trayRussian() ? "Не удалось изменить автозапуск" : "Could not change autostart");
    });
    m.addSeparator();
    m.addItem(tr("Выход", "Quit"), [this] { if (a_.quit) a_.quit(); });
#if defined(__APPLE__)
    showDropdownMenu(m); // native NSMenu under the status item
#else
    // Foreground first, or the menu does not close when the user clicks elsewhere.
    juce::Process::makeForegroundProcess();
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
#endif
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

} // namespace ks::app
