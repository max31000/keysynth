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
#include <cstdlib>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <climits>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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

void initApplication(bool) {}
void installAppEventHandlers() {}
bool takeAppReopenRequest() { return false; }
bool takeAppQuitRequest() { return false; }
bool appLaunchedByUrl() { return false; }

#else // !_WIN32

namespace {

std::string runtimeDir() {
    std::string d;
#if defined(__APPLE__)
    // The per-user temp dir (/var/folders/...), also when TMPDIR is unset (sudo, some launchd / ssh contexts).
    char buf[PATH_MAX];
    if (confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf)) > 0) d = buf;
#endif
    if (d.empty()) {
        const char* t = std::getenv("TMPDIR");
        d = t && *t ? t : "/tmp";
    }
    if (d.back() != '/') d += '/';
    return d;
}

std::string basePath(const std::string& name) { return runtimeDir() + name + "-" + std::to_string(getuid()); }

bool fillAddr(sockaddr_un& a, const std::string& path) {
    a = {};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof(a.sun_path)) return false;
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return true;
}

} // namespace

SingleInstance::SingleInstance(const std::string& name) {
    const std::string base = basePath(name);
    lockFd_ = ::open((base + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    // A lock file we can't create counts as "not running" (as an unopenable mutex on Windows).
    primary_ = lockFd_ < 0 || ::flock(lockFd_, LOCK_EX | LOCK_NB) == 0;
    if (!primary_ || lockFd_ < 0) return;
    sockPath_ = base + ".sock";
    sockaddr_un a;
    if (!fillAddr(a, sockPath_)) return;
    ::unlink(sockPath_.c_str()); // stale socket of a crashed primary (we hold the lock: nobody else owns it)
    sockFd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sockFd_ < 0) return;
    ::fcntl(sockFd_, F_SETFL, O_NONBLOCK);
    ::fcntl(sockFd_, F_SETFD, FD_CLOEXEC);
    if (::bind(sockFd_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(sockFd_);
        sockFd_ = -1;
    }
}

SingleInstance::~SingleInstance() {
    if (sockFd_ >= 0) {
        ::close(sockFd_);
        ::unlink(sockPath_.c_str());
    }
    if (lockFd_ >= 0) ::close(lockFd_); // releases the flock
}

void SingleInstance::drain() {
    if (sockFd_ < 0) return;
    char buf[16];
    for (;;) {
        const ssize_t n = ::recv(sockFd_, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (buf[0] == 'o') open_ = true;
        else if (buf[0] == 'q') quit_ = true;
    }
}

bool SingleInstance::takeOpenRequest() {
    if (!primary_) return false;
    drain();
    return std::exchange(open_, false);
}

bool SingleInstance::takeQuitRequest() {
    if (!primary_) return false;
    drain();
    return std::exchange(quit_, false);
}

bool SingleInstance::send(Command c, const std::string& name) {
    sockaddr_un a;
    if (!fillAddr(a, basePath(name) + ".sock")) return false;
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    const char msg = c == Command::Open ? 'o' : 'q';
    const bool ok = ::sendto(fd, &msg, 1, 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 1;
    ::close(fd);
    return ok;
}

bool SingleInstance::isRunning(const std::string& name) {
    const int fd = ::open((basePath(name) + ".lock").c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd); // also drops our lock if we just took it
    return held;
}

#if !defined(__APPLE__) // macOS: DesktopIntegration_mac.mm
bool autostartEnabled(const std::string&, const std::string&) { return false; }
bool setAutostart(bool, const std::filesystem::path&, const std::string&, const std::string&) { return false; }
void initApplication(bool) {}
void installAppEventHandlers() {}
bool takeAppReopenRequest() { return false; }
bool takeAppQuitRequest() { return false; }
bool appLaunchedByUrl() { return false; }
#endif

bool openInBrowser(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return false;
    return juce::URL(juce::String::fromUTF8(url.c_str())).launchInDefaultBrowser();
}

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
