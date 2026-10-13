// keysynth-engine: JUCE app without windows (optional tray icon). Wiring only — see docs/ARCHITECTURE.md §2, §16.
//   --tray               desktop mode without opening the browser (autostart). Installed mode defaults to desktop
//                        mode + open the UI. Desktop mode = tray icon + single instance (named mutex / lock file).
//   --no-tray            no tray / single instance even when installed (headless, CI, tests)
//   --quit               ask the running desktop instance to quit (exit code 1 if none), then exit
//   --version            print "keysynth X.Y.Z" and exit
//   keysynth://...       protocol launch (web page "start" button): desktop mode without opening the browser
//   --no-audio           drive the engine from a timer thread (null device), for UI/protocol work
//   --port N             WebSocket port (default 7341); --http-port N static UI port (default 7340, 0 = off)
//   --buffer N           request this buffer size from the audio device (alias: --asio-buffer)
//   --sample-rate N      request this sample rate
//   --preset PATH        initial preset (relative to repo root or absolute, inside the allow-list)
//   --root DIR           repo root (default: auto-detected)
//   --no-midi            do not open MIDI inputs
//   --no-plugins         do not load/watch plugins/ (Faust JIT + DLL plugins, docs/PLUGINS.md)
//   --no-cloud           no cloud link / sample downloads / preset sync
//   --cloud-api URL      API base (dev builds only; also KS_CLOUD_API, alias --cloud-base / KS_CLOUD_BASE); the
//                        relay host connects to <URL>/api/relay while linked
//   --web-origin ORIGIN  extra `web` origin (dev builds only; also KS_WEB_ORIGIN)

#include "app/DesktopShell.h"
#include "app/Tray.h"
#include "audio/AudioHost.h"
#include "audio/MidiHub.h"
#include "audio/NullAudioDriver.h"
#include "cloud/CloudServices.h"
#include "control/ControlServer.h"
#include "control/PortConflict.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/RtCheck.h"
#include "platform/DesktopIntegration.h"
#include "platform/PortOwner.h"
#include "plugins/PluginHost.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace {

std::atomic<bool> g_quit{false};
extern "C" void onSignal(int) { g_quit.store(true); }

struct Args {
    bool noAudio = false, noMidi = false, noPlugins = false, noCloud = false, help = false, version = false;
    bool tray = false, noTray = false, quit = false, protocol = false;
    int wsPort = 7341, httpPort = 7340;
    std::optional<int> buffer;
    std::optional<double> sampleRate;
    std::string preset, root, cloudApi, webOrigin;
};

std::string envOr(const char* a, const char* b) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    const char* v = std::getenv(a);
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    if (!v || !*v) v = b ? std::getenv(b) : nullptr;
    return v ? std::string(v) : std::string();
}

Args parseArgs(int argc, char** argv, std::string& err) {
    Args a;
    if (ks::platform::isProtocolLaunch(argc, argv)) { // every other argument is ignored (shell-split URL)
        a.protocol = true;
        return a;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                err = "missing value for " + s;
                return {};
            }
            return argv[++i];
        };
        try {
            if (s == "--no-audio") a.noAudio = true;
            else if (s == "--no-midi") a.noMidi = true;
            else if (s == "--no-plugins") a.noPlugins = true;
            else if (s == "--port") a.wsPort = std::stoi(next());
            else if (s == "--http-port") a.httpPort = std::stoi(next());
            else if (s == "--asio-buffer" || s == "--buffer") a.buffer = std::stoi(next());
            else if (s == "--sample-rate") a.sampleRate = std::stod(next());
            else if (s == "--preset") a.preset = next();
            else if (s == "--root") a.root = next();
            else if (s == "--no-cloud") a.noCloud = true;
            else if (s == "--cloud-api" || s == "--cloud-base") a.cloudApi = next();
            else if (s == "--web-origin") a.webOrigin = next();
            else if (s == "--tray") a.tray = true;
            else if (s == "--no-tray") a.noTray = true;
            else if (s == "--quit") a.quit = true;
            else if (s == "--help" || s == "-h") a.help = true;
            else if (s == "--version") a.version = true;
            else err = "unknown argument " + s;
        } catch (...) {
            err = "bad value for " + s;
        }
        if (!err.empty()) break;
    }
    return a;
}

class Pump final : public juce::Timer {
public:
    Pump(ks::Session& s, ks::ControlServer& srv, ks::plugins::PluginHost* plugins, ks::cloud::CloudServices* cloud)
        : s_(s), srv_(srv), plugins_(plugins), cloud_(cloud) { startTimerHz(30); }
    void timerCallback() override {
        if (ks::platform::takeAppQuitRequest()) g_quit.store(true); // macOS quit Apple Event (logout, ...)
        if (g_quit.load()) {
            stopTimer();
            juce::MessageManager::getInstance()->stopDispatchLoop();
            return;
        }
        s_.engine().collectGarbage(); // every 33 ms (ARCHITECTURE: <= 50 ms)
        if (plugins_) plugins_->poll(); // finished compiles -> registry; file watch every 500 ms; faults
        s_.pollModuleErrors();          // failed sample loads etc. -> `log` error events
        if (cloud_) srv_.dispatchAsync(cloud_->poll()); // cloud replies, sync/sample events
        if (s_.pollSongKeys()) { // song control keys -> section switch (ARCHITECTURE §18)
            srv_.broadcast(s_.songStateJson());
            srv_.broadcast(s_.stateJson());
        }
        const auto midi = s_.pollMidi();
        const auto tele = s_.pollTelemetry();
        updateLive(tele);
        srv_.tickRemote(); // coalesced params / midi batch of relayed controllers
        if (srv_.clientCount() == 0) return;
        if (!midi.is_null()) srv_.broadcast(midi);
        srv_.broadcast(tele);
    }

    // Latest audio telemetry for the macOS menu-bar widget: peaks held and decayed so a 4 Hz reader sees them.
    struct Live {
        float cpu = 0.0f;
        int voices = 0;
        float masterDb = -120.0f;
    };
    const Live& live() const noexcept { return live_; }

private:
    void updateLive(const nlohmann::json& t) {
        const float cpu = t.value("cpu", 0.0f);
        live_.cpu = std::max(cpu, live_.cpu * 0.92f);
        live_.voices = t.value("voices", 0);
        float db = -120.0f;
        if (auto m = t.find("meters"); m != t.end())
            if (auto ma = m->find("master"); ma != m->end() && ma->is_array())
                for (const auto& v : *ma)
                    if (v.is_number()) db = std::max(db, v.get<float>());
        live_.masterDb = std::max(db, live_.masterDb - 1.5f); // ~45 dB/s fall
    }

    ks::Session& s_;
    ks::ControlServer& srv_;
    ks::plugins::PluginHost* plugins_;
    ks::cloud::CloudServices* cloud_;
    Live live_;
};

void printStatus(const ks::AudioStatus& st, const char* prefix) {
    std::printf("%s device: %s / %s\n", prefix, st.type.c_str(), st.name.c_str());
    std::printf("%s sample rate: %.0f Hz, buffer: %d samples (%.2f ms)\n", prefix, st.sampleRate, st.bufferSize,
                st.sampleRate > 0 ? 1000.0 * st.bufferSize / st.sampleRate : 0.0);
    std::printf("%s latency: input %.2f ms, output %.2f ms (device-reported)\n", prefix, st.inputLatencyMs,
                st.outputLatencyMs);
    std::fflush(stdout);
}

// A [::1] listener does not block our 127.0.0.1 bind, but clients using `localhost` may reach it instead.
void logShadowListeners(int port) {
    for (const auto& l : ks::shadowListeners(ks::platform::tcpListeners(port)))
        std::printf("control warning: %s (PID %lu) also listens on [%s]:%d; clients using 'localhost' may reach it "
                    "instead of keysynth (the UI always uses 127.0.0.1)\n",
                    l.image.empty() ? "?" : l.image.c_str(), l.pid, l.address.c_str(), port);
    std::fflush(stdout);
}

// Desktop mode, a port could not be bound (ARCHITECTURE §16 "Port busy at start"): retry every 3 s, balloon on a
// new owner and once bound. Message thread.
class PortWatch final : public juce::Timer {
public:
    using Notify = std::function<void(const std::string& title, const std::string& text)>;
    PortWatch(ks::ControlServer& srv, Notify notify, std::function<void()> onWsBound)
        : srv_(srv), notify_(std::move(notify)), onWsBound_(std::move(onWsBound)) {
        if (const auto& c = srv_.wsConflict()) lastPid_ = c->pid;
        if (!srv_.wsRunning() || srv_.httpConflict()) startTimer(3000);
    }
    void timerCallback() override {
        const bool hadWs = srv_.wsRunning(), hadHttp = srv_.httpRunning();
        srv_.retryListen();
        const bool ru = ks::app::trayRussian();
        if (!hadWs && srv_.wsRunning()) {
            std::printf("control: ws://127.0.0.1:%d bound\n", srv_.wsPort());
            logShadowListeners(srv_.wsPort());
            notify_("keysynth", ks::portFreeMessage(srv_.wsPort(), ru));
            if (onWsBound_) onWsBound_();
        } else if (const auto& c = srv_.wsConflict(); c && c->pid != lastPid_) {
            lastPid_ = c->pid; // another program took the port meanwhile
            std::printf("control: %s\n", ks::conflictLogLine(*c).c_str());
            notify_("keysynth", ks::conflictMessage(*c, ru));
        }
        if (!hadHttp && srv_.httpRunning()) std::printf("control: ui http://127.0.0.1:%d bound\n", srv_.httpPort());
        std::fflush(stdout);
        if (srv_.wsRunning() && !srv_.httpConflict()) stopTimer();
    }
    void notifyConflict() const {
        if (const auto& c = srv_.wsConflict()) notify_("keysynth", ks::conflictMessage(*c, ks::app::trayRussian()));
    }
    // Short tooltip line (Windows cuts the tooltip at 127 UTF-16 units), "" when everything is bound.
    std::string tooltipLine() const {
        const bool ru = ks::app::trayRussian();
        auto line = [ru](const ks::PortConflict& c) {
            const std::string who = c.kind == ks::PortOwnerKind::Unknown ? std::string("?")
                                    : (c.name.empty() ? std::string("PID ") : c.name + " PID ") + std::to_string(c.pid);
            return (ru ? "\nПорт " : "\nPort ") + std::to_string(c.port) + (ru ? " занят: " : " busy: ") + who;
        };
        std::string t;
        if (const auto& c = srv_.wsConflict()) t += line(*c);
        if (const auto& c = srv_.httpConflict()) t += line(*c);
        return t;
    }

private:
    ks::ControlServer& srv_;
    Notify notify_;
    std::function<void()> onWsBound_;
    unsigned long lastPid_ = 0;
};

std::string trayTooltip(const ks::Session& session, ks::cloud::CloudServices* cloud, const std::string& portLine) {
    const bool ru = ks::app::trayRussian();
    std::string t = std::string("keysynth ") + ks::engineVersion() + portLine;
    if (const ks::AudioControl* a = session.audio()) {
        const ks::AudioStatus st = a->status();
        if (st.running) {
            // Truncate by characters, not bytes (Cyrillic device names are multi-byte UTF-8).
            const std::string dev = juce::String::fromUTF8(st.name.c_str()).substring(0, 40).toStdString();
            char line[96];
            std::snprintf(line, sizeof(line), "\n%.1f kHz, %d (%.1f ms)", st.sampleRate / 1000.0, st.bufferSize,
                          st.sampleRate > 0 ? 1000.0 * st.bufferSize / st.sampleRate : 0.0);
            t += "\n" + st.type + ": " + dev + line;
        } else {
            t += ru ? "\nЗвук: нет устройства" : "\nAudio: no device";
        }
    }
    if (cloud) {
        const auto cs = cloud->client().status();
        const std::string who = juce::String::fromUTF8(cs.accountName.c_str()).substring(0, 24).toStdString();
        if (cs.linked) t += (ru ? "\nОблако: " : "\nCloud: ") + who;
        else t += ru ? "\nОблако: не подключено" : "\nCloud: not linked";
    }
    return t;
}

} // namespace

int main(int argc, char** argv) {
    std::string err;
    const Args args = parseArgs(argc, argv, err);
    if (!err.empty() || args.help) {
        if (!err.empty()) std::fprintf(stderr, "error: %s\n", err.c_str());
        std::printf("usage: keysynth-engine [--no-audio] [--no-midi] [--port N] [--http-port N] [--buffer N]\n"
                    "                       [--sample-rate HZ] [--preset PATH] [--root DIR] [--no-plugins] [--no-cloud]\n"
                    "                       [--tray | --no-tray] [--quit] [--version] [keysynth://open]\n");
        return err.empty() ? 0 : 2;
    }
    if (args.version) {
        std::printf("keysynth %s\n", ks::engineVersion());
        return 0;
    }
    if (args.quit) return ks::platform::SingleInstance::send(ks::platform::SingleInstance::Command::Quit) ? 0 : 1;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if defined(SIGBREAK)
    std::signal(SIGBREAK, onSignal);
#endif

    ks::platform::initApplication(false); // macOS: NSApp before JUCE (see DesktopIntegration.h)
    juce::ScopedJuceInitialiser_GUI juceInit; // message manager (+ the tray icon in desktop mode)
    ks::platform::installAppEventHandlers();

    const auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto exeDir = exeFile.getParentDirectory();
    // UTF-8 -> path: the install dir is under the user's profile, often non-ASCII (C:/Users/<Cyrillic name>/...).
    const ks::AppPaths paths =
        args.root.empty() ? ks::AppPaths::discover(ks::pathFromUtf8(exeDir.getFullPathName().toStdString()))
                          : ks::AppPaths::fromRoot(args.root);

    // Desktop mode (ARCHITECTURE §16): tray + single instance. A second launch hands over to the first and exits.
    const auto mode = ks::platform::desktopMode(paths.installed(), args.tray, args.noTray, args.protocol);
    const bool desktop = mode.desktop, openOnStart = mode.openOnStart;
    const std::string localUrl = ks::platform::localUiUrl(args.httpPort, args.wsPort);
    std::unique_ptr<ks::platform::SingleInstance> instance;
    if (desktop) {
        instance = std::make_unique<ks::platform::SingleInstance>();
        if (!instance->isPrimary()) {
            std::printf("keysynth is already running%s\n", openOnStart ? ": opening the UI" : "");
            if (openOnStart && !ks::platform::SingleInstance::send(ks::platform::SingleInstance::Command::Open)) {
                const bool web = ks::platform::probeHealth(ks::platform::kWebHealthUrl, 3000);
                ks::platform::openInBrowser(web ? ks::platform::kWebUiUrl : localUrl);
            }
            return 0;
        }
    }
    if (desktop) ks::platform::initApplication(true); // macOS: menu bar app, no Dock icon
    // After the instance check: only the primary desktop instance rotates the log.
    ks::app::setupGuiOutput(paths.logsDir, desktop);
#if defined(__APPLE__)
    // The Windows installer copies the example plugins into the data dir (onlyifdoesntexist); a dragged-in .app
    // has no installer, so the first start does it (never over an existing plugins dir).
    if (paths.installed()) {
        std::error_code ec;
        const auto examples = paths.root / "plugins";
        if (std::filesystem::is_directory(examples, ec) && !std::filesystem::exists(paths.pluginsDir, ec)) {
            std::filesystem::copy(examples, paths.pluginsDir, std::filesystem::copy_options::recursive, ec);
            if (ec) std::fprintf(stderr, "plugins: could not copy the examples: %s\n", ec.message().c_str());
        }
    }
#endif
    std::printf("keysynth-engine — root %s%s\n", ks::pathToUtf8(paths.root).c_str(),
                ks::rt::checksEnabled() ? " (RT checks on)" : "");

    auto engine = std::make_unique<ks::Engine>();
    ks::Session session(*engine, ks::defaultRegistry(), paths);
    ks::ProtocolHandler handler(session);
    ks::ControlServer server(
        handler, [](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); }, paths.uiDist);
    {
        ks::OriginPolicy op;
        op.devBuild = !paths.installed();
        if (op.devBuild) op.extraWebOrigin = args.webOrigin.empty() ? envOr("KS_WEB_ORIGIN", nullptr) : args.webOrigin;
        server.setOriginPolicy(op);
    }
    session.log = [&server](const std::string& level, const std::string& msg, bool notify) {
        std::printf("[%s] %s\n", level.c_str(), msg.c_str());
        std::fflush(stdout);
        nlohmann::json ev = {{"type", "log"}, {"level", level}, {"message", msg}};
        if (notify) ev["notify"] = true;
        server.broadcast(std::move(ev));
    };

    // Check the WS port before starting audio/plugins/cloud, so an early exit is cheap. The bind itself comes after
    // the setup (a client's hello must not wait for a slow ASIO open and time out).
    if (const auto busy = ks::ControlServer::probe(args.wsPort)) {
        std::printf("control: %s\n", ks::conflictLogLine(*busy).c_str());
        std::fflush(stdout);
        if (!desktop) { // headless (dev, scripts, CI): fail fast with the classified reason
            ks::app::reportFatal("keysynth: control server: " + ks::conflictLogLine(*busy));
            return 1;
        }
        if (busy->kind == ks::PortOwnerKind::Keysynth) {
            // Another engine outside our single instance (headless/dev build, other session) serves the UI.
            std::printf("another keysynth engine owns port %d%s\n", args.wsPort, openOnStart ? ": opening the UI" : "");
            if (openOnStart) {
                const bool web = ks::platform::probeHealth(ks::platform::kWebHealthUrl, 3000);
                ks::platform::openInBrowser(web ? ks::platform::kWebUiUrl : localUrl);
            }
            return 0;
        }
        // Foreign owner: keep running in the tray and retry (PortWatch below), no modal box.
    }

    // Plugins compile asynchronously; patches referencing them are rebuilt when they become available.
    std::unique_ptr<ks::plugins::PluginHost> plugins;
    if (!args.noPlugins) {
        ks::plugins::PluginHost::Options popt;
        popt.pluginsDir = paths.pluginsDir;
        popt.buildDir = paths.pluginBuildDir;
        plugins = std::make_unique<ks::plugins::PluginHost>(ks::defaultRegistry(), popt);
        plugins->onStatus = [&server, &session](const ks::plugins::PluginStatus& st) {
            if (st.state == "error" || st.state == "faulted")
                session.log("error", "plugin " + st.name + ": " + st.message, false); // shown by the plugin toast
            else if (st.state == "ok")
                std::printf("[plugin] %s v%d ok (%s, %.0f ms%s)\n", st.name.c_str(), st.version,
                            st.source.c_str(), st.compileMs, st.cached ? ", cached" : "");
            std::fflush(stdout);
            server.broadcast({{"type", "plugin_status"}, {"plugin", ks::plugins::toJson(st)}});
        };
        plugins->onModulesChanged = [&server, &session](const std::vector<std::string>& typeIds) {
            if (session.refreshModules(typeIds)) server.broadcast(session.stateJson());
        };
        session.setPlugins(plugins.get());
        const auto fi = ks::plugins::PluginHost::faustInfo();
        std::printf("plugins: %s (faust %s)\n", ks::pathToUtf8(popt.pluginsDir).c_str(),
                    fi["available"].get<bool>() ? fi["version"].get<std::string>().c_str()
                                                : ("unavailable: " + fi["reason"].get<std::string>()).c_str());
        plugins->start();
    }

    // Cloud link, sample cache, preset sync (ARCHITECTURE §15).
    std::unique_ptr<ks::cloud::CloudServices> cloud;
    if (!args.noCloud) {
        ks::cloud::CloudServices::Options copt;
        if (!paths.installed()) {
            const std::string api = args.cloudApi.empty() ? envOr("KS_CLOUD_API", "KS_CLOUD_BASE") : args.cloudApi;
            if (!api.empty()) copt.baseUrl = api;
        }
        cloud = std::make_unique<ks::cloud::CloudServices>(session, copt);
        handler.setCloud(cloud.get());
        cloud->start();
        // Relay host (ARCHITECTURE §15 *Relay*): remote controllers become virtual clients of the server.
        ks::cloud::bindRelay(cloud->relay(), server,
                             [](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); });
        const auto cs = cloud->client().status();
        std::printf("cloud: %s, %s (device %s), data %s\n", copt.baseUrl.c_str(),
                    cs.linked ? ("linked to " + cs.accountName).c_str() : "not linked", cs.deviceId.c_str(),
                    ks::pathToUtf8(paths.dataRoot).c_str());
    }

    if (!args.preset.empty()) {
        try {
            session.loadPreset(args.preset);
            std::printf("preset: %s\n", session.presetPath().c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "cannot load preset %s: %s\n", args.preset.c_str(), e.what());
        }
    }

    std::unique_ptr<ks::NullAudioDriver> nullAudio;
    std::unique_ptr<ks::AudioHost> audioHost;
    auto onPrepared = [&session] { session.rebuild(false); };
    if (args.noAudio) {
        nullAudio = std::make_unique<ks::NullAudioDriver>(*engine, onPrepared, args.sampleRate.value_or(48000.0),
                                                          args.buffer.value_or(256));
        nullAudio->start();
        session.setAudio(nullAudio.get());
        printStatus(nullAudio->status(), "audio");
    } else {
        audioHost = std::make_unique<ks::AudioHost>(*engine, paths, onPrepared);
        // Device events (driver restart, stall, recovery, errors): also in userdata/logs/audio.log.
        audioHost->onLog = [&session](const std::string& level, const std::string& msg, bool notify) {
            session.log(level, msg, notify);
        };
        ks::AudioHost::Options opt;
        opt.bufferSize = args.buffer;
        opt.sampleRate = args.sampleRate;
        const std::string aerr = audioHost->start(opt);
        if (!aerr.empty()) std::fprintf(stderr, "audio: %s\n", aerr.c_str());
        session.setAudio(audioHost.get());
        const auto st = audioHost->status();
        printStatus(st, "audio");
        std::printf("audio device log: %s\n", ks::pathToUtf8(audioHost->logFile()).c_str());
        std::string sizes;
        for (const auto& t : audioHost->listDevices().available)
            if (t.type == st.type)
                for (int b : t.bufferSizes) sizes += (sizes.empty() ? "" : ", ") + std::to_string(b);
        std::printf("audio buffer sizes offered by the driver: %s\n", sizes.empty() ? "?" : sizes.c_str());
        if (args.buffer && st.bufferSize != *args.buffer)
            std::printf("audio note: requested buffer %d not accepted by the driver (ASIO drivers often only offer the "
                        "size set in their control panel: UI 'Open ASIO panel')\n",
                        *args.buffer);
        // Device restarted / panel closed: re-report to every client (buffer size may have changed). The host logs
        // the change itself (onLog).
        audioHost->onChanged = [&session, &server] {
            if (server.clientCount() == 0) return;
            server.broadcast(session.devicesJson());
            server.broadcast(session.stateJson());
        };
    }

    std::unique_ptr<ks::MidiHub> midi;
    if (!args.noMidi) {
        midi = std::make_unique<ks::MidiHub>(*engine);
        midi->rescan();
        session.setMidi(midi.get());
        std::printf("midi inputs:");
        for (const auto& n : midi->inputs()) std::printf(" [%s]", n.c_str());
        std::printf("\n");
    }

    server.start(args.wsPort, args.httpPort);
    logShadowListeners(args.wsPort);
    if (const auto& busy = server.wsConflict()) {
        std::printf("control: %s\n", ks::conflictLogLine(*busy).c_str());
        if (!desktop) { // taken meanwhile
            ks::app::reportFatal("keysynth: control server: " + ks::conflictLogLine(*busy));
            return 1;
        }
    }
    if (server.wsRunning())
        std::printf("control: ws://127.0.0.1:%d%s\n", args.wsPort,
                    server.httpRunning() ? (", ui: http://127.0.0.1:" + std::to_string(args.httpPort)).c_str()
                    : args.httpPort == 0 ? " (static UI disabled)"
                    : server.httpConflict() ? " (static UI port busy, retrying)"
                                            : " (ui/dist not built: use the Vite dev server)");
    else
        std::printf("control: waiting for port %d (retry every 3 s)\n", args.wsPort);
    std::printf("ready. Ctrl+C to quit.\n");
    std::fflush(stdout);

    Pump pump(session, server, plugins.get(), cloud.get());

    std::unique_ptr<ks::app::UiOpener> opener;
    std::unique_ptr<ks::app::Tray> tray;
    std::unique_ptr<PortWatch> portWatch;
    bool openWhenBound = false; // open-on-start (or an open request) deferred until the WS port is ours
    if (desktop) {
        opener = std::make_unique<ks::app::UiOpener>(localUrl, [&server] { return server.httpRunning(); });
        portWatch = std::make_unique<PortWatch>(
            server, [&tray](const std::string& title, const std::string& text) { if (tray) tray->notify(title, text); },
            [&openWhenBound, o = opener.get()] {
                if (std::exchange(openWhenBound, false)) o->openPreferred();
            });
        // Never open a browser while another program answers on the WS port: it would talk to that program.
        auto openUi = [&server, &openWhenBound, o = opener.get(), w = portWatch.get()] {
            if (server.wsRunning()) return o->openPreferred();
            openWhenBound = true;
            w->notifyConflict();
        };
        ks::app::TrayActions ta;
        ta.tooltip = [&session, c = cloud.get(), w = portWatch.get()] { return trayTooltip(session, c, w->tooltipLine()); };
        ta.openUi = openUi;
        ta.openLocal = [o = opener.get()] { o->openLocal(); };
        ta.localAvailable = [&server] { return server.httpRunning(); };
        ta.restartAudio = [&session, &server]() -> std::string {
            ks::AudioControl* a = session.audio();
            if (!a) return "no audio host";
            if (a->status().panelOpen) return "close the driver's control panel first";
            std::string e = a->restart();
            if (server.clientCount() > 0) {
                server.broadcast(session.devicesJson());
                server.broadcast(session.stateJson());
            }
            return e;
        };
        const std::filesystem::path exePath = ks::pathFromUtf8(exeFile.getFullPathName().toStdString());
        ta.autostartEnabled = [] { return ks::platform::autostartEnabled(); };
        ta.setAutostart = [exePath](bool on) { return ks::platform::setAutostart(on, exePath); };
        ta.quit = [] { g_quit.store(true); };
        ta.openLogs = [dir = paths.logsDir] {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const juce::File d(juce::String::fromUTF8(ks::pathToUtf8(dir).c_str()));
            const juce::File log = d.getChildFile("engine.log");
            (log.existsAsFile() ? log : d).revealToUser();
        };
        ta.status = [&session, &server, &pump, c = cloud.get(), &args] {
            ks::platform::MenuBarStatus s;
            s.version = ks::engineVersion();
            s.wsPort = args.wsPort;
            s.engineOk = server.wsRunning();
            if (const auto& busy = server.wsConflict()) s.portProblem = ks::conflictMessage(*busy, ks::app::trayRussian());
            s.remoteClients = static_cast<int>(server.remoteCount());
            s.localClients = static_cast<int>(server.clientCount()) - s.remoteClients;
            if (const ks::AudioControl* a = session.audio()) {
                const ks::AudioStatus st = a->status();
                s.audioRunning = st.running;
                s.audioType = st.type;
                s.audioDevice = st.name;
                s.sampleRate = st.sampleRate;
                s.bufferSize = st.bufferSize;
                s.outputLatencyMs = st.outputLatencyMs;
            }
            s.cpu = pump.live().cpu;
            s.voices = pump.live().voices;
            s.masterDb = pump.live().masterDb;
            s.preset = session.model().patch().meta.name;
            if (const auto* m = session.midi()) s.midiInputs = m->inputs();
            if (c) {
                const auto cs = c->client().status();
                s.cloudEnabled = true;
                s.cloudLinked = cs.linked;
                s.cloudAccount = cs.accountName;
            }
            s.localAvailable = server.httpRunning();
            s.autostart = ks::platform::autostartEnabled();
            return s;
        };
        ta.poll = [inst = instance.get(), openUi] {
            if (inst->takeQuitRequest()) g_quit.store(true);
            if (inst->takeOpenRequest() || ks::platform::takeAppReopenRequest()) openUi();
        };
        tray = std::make_unique<ks::app::Tray>(std::move(ta), ks::engineVersion());
        if (!server.wsRunning()) {
            openWhenBound = openOnStart;
            portWatch->notifyConflict(); // balloon instead of a modal box
        } else if (openOnStart) {
#if defined(__APPLE__)
            // A `keysynth://` link launch arrives as an Apple Event once the run loop starts, not in argv: wait
            // for it briefly, a link launch must not open a browser (DesktopMode).
            juce::Timer::callAfterDelay(500, [o = opener.get()] {
                if (!ks::platform::appLaunchedByUrl()) o->openPreferred();
            });
#else
            opener->openPreferred();
#endif
        }
    }
#if defined(_WIN32) // elsewhere the driver's IO thread is already real-time (CoreAudio HAL): nothing to report
    if (audioHost)
        juce::Timer::callAfterDelay(1000, [host = audioHost.get()] {
            std::printf("audio thread: MMCSS \"Pro Audio\" %s (err %lu), power throttling %s\n",
                        host->mmcssActive() ? "on" : "off", host->mmcssError(),
                        host->powerThrottlingDisabled() ? "disabled" : "unchanged");
            std::fflush(stdout);
        });
#endif
    juce::MessageManager::getInstance()->runDispatchLoop();

    std::printf("shutting down...\n");
    juce::PopupMenu::dismissAllActiveMenus();
    portWatch.reset(); // before the tray it notifies
    tray.reset();
    opener.reset(); // joins a running health probe (<= 3 s)
    pump.stopTimer();
    server.stop();
    handler.setCloud(nullptr);
    if (cloud) cloud->stop();
    if (midi) midi->closeAll();
    if (audioHost) audioHost->stop();
    if (nullAudio) nullAudio->stop();
    session.setPlugins(nullptr);
    plugins.reset();
    engine->collectGarbage();
    return 0;
}
