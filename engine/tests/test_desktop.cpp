// Desktop integration (platform/DesktopIntegration, ARCHITECTURE §16): single instance + commands, autostart
// registry value, URL helpers. Uses per-process names / a private HKCU key, never the real Run key or mutex.
#include "platform/DesktopIntegration.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace ks::platform;

namespace {
std::string uniqueName(const char* what) {
#if defined(_WIN32)
    return std::string("keysynth-test-") + what + "-" + std::to_string(GetCurrentProcessId());
#else
    return std::string("keysynth-test-") + what + "-" + std::to_string(getpid());
#endif
}
} // namespace

TEST_CASE("desktop: URL helpers and protocol args", "[desktop]") {
    CHECK(localUiUrl(7340, 7341) == "http://127.0.0.1:7340/");
    CHECK(localUiUrl(7350, 7351) == "http://127.0.0.1:7350/?engine=127.0.0.1:7351");
    CHECK(isProtocolArg("keysynth://open"));
    CHECK(isProtocolArg("keysynth://open/"));
    CHECK(isProtocolArg("KeySynth:open"));
    CHECK_FALSE(isProtocolArg("keysynth"));
    CHECK_FALSE(isProtocolArg("--tray"));
    CHECK_FALSE(isProtocolArg("https://keysynth/"));
    CHECK(autostartCommand(std::filesystem::path(L"C:\\Apps\\key synth\\keysynth-engine.exe")) ==
          "\"C:\\Apps\\key synth\\keysynth-engine.exe\" --tray");
    CHECK(std::string(kInstanceName) == "keysynth-engine"); // = AppMutex in installer/keysynth.iss
}

TEST_CASE("desktop: mode decision and protocol launches", "[desktop]") {
    auto m = desktopMode(false, false, false, false); // dev, no flags: headless as before
    CHECK_FALSE(m.desktop);
    CHECK_FALSE(m.openOnStart);
    m = desktopMode(true, false, false, false); // installed, Start menu
    CHECK(m.desktop);
    CHECK(m.openOnStart);
    m = desktopMode(true, true, false, false); // autostart --tray
    CHECK(m.desktop);
    CHECK_FALSE(m.openOnStart);
    m = desktopMode(true, false, false, true); // keysynth://open
    CHECK(m.desktop);
    CHECK_FALSE(m.openOnStart);
    CHECK_FALSE(desktopMode(true, true, true, true).desktop); // --no-tray wins
    CHECK(desktopMode(false, true, false, false).desktop);    // dev --tray

    const char* plain[] = {"exe", "--tray", "--port", "7341"};
    CHECK_FALSE(isProtocolLaunch(4, plain));
    // A URL split by the shell into extra flags: still a protocol launch (main ignores the rest).
    const char* split[] = {"exe", "keysynth://open/x", "--root", "C:\\evil", "--preset", "p.json"};
    CHECK(isProtocolLaunch(6, split));
}

TEST_CASE("desktop: health probe fails fast on a closed port", "[desktop]") {
    CHECK_FALSE(probeHealth("http://127.0.0.1:1/health", 1000));
}

TEST_CASE("desktop: single instance and cross-process commands", "[desktop]") {
    const std::string name = uniqueName("instance");
    CHECK_FALSE(SingleInstance::isRunning(name));
    CHECK_FALSE(SingleInstance::send(SingleInstance::Command::Open, name)); // nobody listening
    {
        SingleInstance first(name);
        REQUIRE(first.isPrimary());
        CHECK(SingleInstance::isRunning(name));
        SingleInstance second(name);
        CHECK_FALSE(second.isPrimary());
        CHECK_FALSE(second.takeOpenRequest()); // only the primary receives commands

        CHECK_FALSE(first.takeOpenRequest());
        CHECK(SingleInstance::send(SingleInstance::Command::Open, name));
        CHECK_FALSE(first.takeQuitRequest());
        CHECK(first.takeOpenRequest());
        CHECK_FALSE(first.takeOpenRequest()); // auto-reset: one command, one take
        CHECK(SingleInstance::send(SingleInstance::Command::Quit, name));
        CHECK(first.takeQuitRequest());
    }
    CHECK_FALSE(SingleInstance::isRunning(name));
    SingleInstance again(name);
    CHECK(again.isPrimary());
}

#if !defined(_WIN32)
TEST_CASE("desktop: a crashed primary's stale socket does not block a new one", "[desktop]") {
    const std::string name = uniqueName("stale");
    {
        SingleInstance first(name);
        REQUIRE(first.isPrimary());
    } // the destructor removes the socket; a crash would leave it: the next primary replaces it
    SingleInstance next(name);
    REQUIRE(next.isPrimary());
    CHECK(SingleInstance::send(SingleInstance::Command::Quit, name));
    CHECK(next.takeQuitRequest());
}

TEST_CASE("desktop: no login item outside an app bundle", "[desktop]") {
    CHECK_FALSE(autostartEnabled());
    CHECK_FALSE(setAutostart(true, "/tmp/keysynth-engine"));
}
#endif

#if defined(_WIN32)

TEST_CASE("desktop: autostart value round trip", "[desktop]") {
    const std::string key = "Software\\keysynth-tests\\" + uniqueName("run");
    const std::filesystem::path exe(L"C:\\Программы\\keysynth\\keysynth-engine.exe");
    CHECK_FALSE(autostartEnabled(key, "keysynth"));
    REQUIRE(setAutostart(true, exe, key, "keysynth"));
    CHECK(autostartEnabled(key, "keysynth"));
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    REQUIRE(RegGetValueW(HKEY_CURRENT_USER, std::wstring(key.begin(), key.end()).c_str(), L"keysynth", RRF_RT_REG_SZ,
                         nullptr, buf, &size) == ERROR_SUCCESS);
    CHECK(std::wstring(buf) == L"\"C:\\Программы\\keysynth\\keysynth-engine.exe\" --tray");
    REQUIRE(setAutostart(false, exe, key, "keysynth"));
    CHECK_FALSE(autostartEnabled(key, "keysynth"));
    CHECK(setAutostart(false, exe, key, "keysynth")); // deleting a missing value is fine
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\keysynth-tests");
}
#endif
