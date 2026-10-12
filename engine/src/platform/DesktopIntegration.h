#pragma once
// Desktop-app integration for the tray app (ARCHITECTURE §16): single instance + cross-process commands, HKCU
// autostart, UI URL choice. Message thread only (probeHealth blocks: call it from a worker). Non-Windows: stubs
// (always primary, no autostart).

#include <filesystem>
#include <string>

namespace ks::platform {

inline constexpr const char* kInstanceName = "keysynth-engine"; // = Inno Setup AppMutex (installer/keysynth.iss)
inline constexpr const char* kWebUiUrl = "https://mvv42.ru/keysynth/";
inline constexpr const char* kWebHealthUrl = "https://mvv42.ru/keysynth/health";
inline constexpr const char* kAutostartValue = "keysynth";
inline constexpr const char* kRunKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Named mutex `<name>` + auto-reset events `<name>-open` / `<name>-quit` (session-local namespace). The first
// process holding the mutex is the primary; later ones send it a command and exit.
class SingleInstance {
public:
    explicit SingleInstance(const std::string& name = kInstanceName);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool isPrimary() const noexcept { return primary_; }
    // Primary: true once per command sent by another process (non-blocking).
    bool takeOpenRequest();
    bool takeQuitRequest();

    enum class Command { Open, Quit };
    // Signal the running primary. false when none is running (or it predates the command).
    static bool send(Command c, const std::string& name = kInstanceName);
    static bool isRunning(const std::string& name = kInstanceName);

private:
    void* mutex_ = nullptr;
    void* openEvent_ = nullptr;
    void* quitEvent_ = nullptr;
    bool primary_ = true;
};

// Autostart = HKCU\<runKey> value `<valueName>` = autostartCommand(exe). `runKey` is a parameter for tests.
std::string autostartCommand(const std::filesystem::path& exe); // "\"<exe>\" --tray"
bool autostartEnabled(const std::string& runKey = kRunKey, const std::string& valueName = kAutostartValue);
// Returns false if the registry write/delete failed.
bool setAutostart(bool enabled, const std::filesystem::path& exe, const std::string& runKey = kRunKey,
                  const std::string& valueName = kAutostartValue);

// `http://127.0.0.1:<httpPort>/` (+ `?engine=127.0.0.1:<wsPort>` for a non-default WS port, as scripts/start.ps1).
std::string localUiUrl(int httpPort, int wsPort);
// GET `url` -> true iff HTTP 200 within timeoutMs. Blocking (WinINet via JUCE); never on the message thread.
bool probeHealth(const std::string& url, int timeoutMs);
// Opens `url` in the default browser (ShellExecute). Only http(s) URLs.
bool openInBrowser(const std::string& url);

// `keysynth:` / `keysynth://...` command-line argument (protocol handler launch from the web page).
bool isProtocolArg(const std::string& arg);
// True if any argument is a protocol URL. Such a launch must ignore every other argument: a URL with unescaped
// quotes/spaces can be split by the shell into extra flags (`--root`, `--preset`, ...).
bool isProtocolLaunch(int argc, const char* const* argv);

struct DesktopMode {
    bool desktop = false;     // tray icon + single instance
    bool openOnStart = false; // open the UI once started
};
// Installed mode or --tray / protocol launch => desktop mode, unless --no-tray. --tray (autostart) and protocol
// launches (the web page started us) do not open the browser.
DesktopMode desktopMode(bool installed, bool tray, bool noTray, bool protocol);

} // namespace ks::platform
