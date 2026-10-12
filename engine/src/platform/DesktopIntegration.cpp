#include "platform/DesktopIntegration.h"

#include <juce_core/juce_core.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#include <algorithm>
#include <cctype>

namespace ks::platform {

#if defined(_WIN32)
namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring eventName(const std::string& name, const char* suffix) { return wide(name + "-" + suffix); }

bool takeEvent(void* ev) { return ev && WaitForSingleObject(static_cast<HANDLE>(ev), 0) == WAIT_OBJECT_0; }

} // namespace

SingleInstance::SingleInstance(const std::string& name) {
    mutex_ = CreateMutexW(nullptr, FALSE, wide(name).c_str());
    // A mutex we can't create or open (e.g. a same-named object of another type) counts as "not running".
    primary_ = mutex_ == nullptr || GetLastError() != ERROR_ALREADY_EXISTS;
    if (primary_) {
        openEvent_ = CreateEventW(nullptr, FALSE, FALSE, eventName(name, "open").c_str());
        quitEvent_ = CreateEventW(nullptr, FALSE, FALSE, eventName(name, "quit").c_str());
    }
}

SingleInstance::~SingleInstance() {
    for (void* h : {openEvent_, quitEvent_, mutex_})
        if (h) CloseHandle(static_cast<HANDLE>(h));
}

bool SingleInstance::takeOpenRequest() { return primary_ && takeEvent(openEvent_); }
bool SingleInstance::takeQuitRequest() { return primary_ && takeEvent(quitEvent_); }

bool SingleInstance::send(Command c, const std::string& name) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName(name, c == Command::Open ? "open" : "quit").c_str());
    if (!ev) return false;
    const bool ok = SetEvent(ev) != 0;
    CloseHandle(ev);
    return ok;
}

bool SingleInstance::isRunning(const std::string& name) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, wide(name).c_str());
    if (!m) return false;
    CloseHandle(m);
    return true;
}

bool autostartEnabled(const std::string& runKey, const std::string& valueName) {
    DWORD type = 0, size = 0;
    const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, wide(runKey).c_str(), wide(valueName).c_str(),
                                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, nullptr, &size);
    return st == ERROR_SUCCESS && size > sizeof(wchar_t);
}

bool setAutostart(bool enabled, const std::filesystem::path& exe, const std::string& runKey,
                  const std::string& valueName) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(runKey).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS st;
    const std::wstring vn = wide(valueName);
    if (enabled) {
        const std::wstring cmd = wide(autostartCommand(exe));
        st = RegSetValueExW(key, vn.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, vn.c_str());
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool openInBrowser(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return false;
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr,
                                                           SW_SHOWNORMAL));
    return r > 32;
}

#else // !_WIN32

SingleInstance::SingleInstance(const std::string&) {}
SingleInstance::~SingleInstance() = default;
bool SingleInstance::takeOpenRequest() { return false; }
bool SingleInstance::takeQuitRequest() { return false; }
bool SingleInstance::send(Command, const std::string&) { return false; }
bool SingleInstance::isRunning(const std::string&) { return false; }
bool autostartEnabled(const std::string&, const std::string&) { return false; }
bool setAutostart(bool, const std::filesystem::path&, const std::string&, const std::string&) { return false; }
bool openInBrowser(const std::string& url) { return juce::URL(juce::String::fromUTF8(url.c_str())).launchInDefaultBrowser(); }

#endif

std::string autostartCommand(const std::filesystem::path& exe) {
    const std::u8string u = exe.u8string();
    return "\"" + std::string(u.begin(), u.end()) + "\" --tray";
}

std::string localUiUrl(int httpPort, int wsPort) {
    std::string url = "http://127.0.0.1:" + std::to_string(httpPort) + "/";
    if (wsPort != 7341) url += "?engine=127.0.0.1:" + std::to_string(wsPort);
    return url;
}

bool probeHealth(const std::string& url, int timeoutMs) {
    juce::WebInputStream in(juce::URL(juce::String::fromUTF8(url.c_str())), false);
    in.withConnectionTimeout(timeoutMs).withNumRedirectsToFollow(0);
    if (!in.connect(nullptr) || in.isError()) return false;
    return in.getStatusCode() == 200;
}

bool isProtocolArg(const std::string& arg) {
    static constexpr char kScheme[] = "keysynth:";
    if (arg.size() < sizeof(kScheme) - 1) return false;
    return std::equal(kScheme, kScheme + sizeof(kScheme) - 1, arg.begin(),
                      [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

bool isProtocolLaunch(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i)
        if (argv[i] && isProtocolArg(argv[i])) return true;
    return false;
}

DesktopMode desktopMode(bool installed, bool tray, bool noTray, bool protocol) {
    DesktopMode m;
    m.desktop = !noTray && (tray || protocol || installed);
    m.openOnStart = m.desktop && !tray && !protocol;
    return m;
}

} // namespace ks::platform
