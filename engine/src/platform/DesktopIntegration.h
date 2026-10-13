#pragma once
// Desktop-app integration for the tray app (ARCHITECTURE §16): single instance + cross-process commands, autostart,
// UI URL choice. Message thread only (probeHealth blocks: call it from a worker).
// Windows: named mutex/events, HKCU Run key. macOS: lock file + datagram socket, SMAppService login item, Apple
// Events (DesktopIntegration_mac.mm). Other POSIX: single instance as macOS, no autostart.

#include <filesystem>
#include <string>

namespace ks::platform {

inline constexpr const char* kInstanceName = "keysynth-engine"; // = Inno Setup AppMutex (installer/keysynth.iss)
inline constexpr const char* kWebUiUrl = "https://mvv42.ru/keysynth/";
inline constexpr const char* kWebHealthUrl = "https://mvv42.ru/keysynth/health";
inline constexpr const char* kAutostartValue = "keysynth";
inline constexpr const char* kRunKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Windows: named mutex `<name>` + auto-reset events `<name>-open` / `<name>-quit` (session-local namespace).
// POSIX: exclusive flock on $TMPDIR/<name>-<uid>.lock + a datagram socket next to it that receives the commands.
// The first process holding the lock is the primary; later ones send it a command and exit.
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
#if defined(_WIN32)
    void* mutex_ = nullptr;
    void* openEvent_ = nullptr;
    void* quitEvent_ = nullptr;
#else
    int lockFd_ = -1;
    int sockFd_ = -1;
    std::string sockPath_;
    bool open_ = false, quit_ = false; // commands drained from the socket, not yet taken
    void drain();
#endif
    bool primary_ = true;
};

// Autostart. Windows: HKCU\<runKey> value `<valueName>` = autostartCommand(exe) (`runKey` is a parameter for
// tests). macOS: the app bundle's login item (SMAppService.mainAppService; the key/value/exe arguments are
// unused) — false outside an app bundle.
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

// macOS application object and Apple Events (no-ops elsewhere). initApplication() must run before JUCE's GUI
// initialiser (JUCE creates NSApp only for START_JUCE_APPLICATION apps, and [NSApp run] needs it); `menuBarApp`
// = no Dock icon (accessory activation policy). installAppEventHandlers() after it: LaunchServices never starts a
// second process for a running app, it sends the running one an Apple Event instead — "reopen" (Finder / Dock
// launch) becomes an open request, kAEGetURL (`keysynth://` link) marks a protocol launch, "quit" (logout,
// shutdown, `osascript quit`) a quit request instead of AppKit's immediate exit().
void initApplication(bool menuBarApp);
void installAppEventHandlers();
bool takeAppReopenRequest(); // true once per reopen event
bool takeAppQuitRequest();   // true once per quit event
bool appLaunchedByUrl();     // a kAEGetURL event has arrived (sticky)

struct DesktopMode {
    bool desktop = false;     // tray icon + single instance
    bool openOnStart = false; // open the UI once started
};
// Installed mode or --tray / protocol launch => desktop mode, unless --no-tray. --tray (autostart) and protocol
// launches (the web page started us) do not open the browser.
DesktopMode desktopMode(bool installed, bool tray, bool noTray, bool protocol);

} // namespace ks::platform
