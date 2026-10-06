// keysynth-engine: headless JUCE app (no window). Wiring only — see docs/ARCHITECTURE.md §2.
//   --no-audio           drive the engine from a timer thread (null device), for UI/protocol work
//   --port N             WebSocket port (default 7341); --http-port N static UI port (default 7340, 0 = off)
//   --asio-buffer N      request this buffer size from the audio device
//   --sample-rate N      request this sample rate
//   --preset PATH        initial preset (relative to repo root or absolute, inside the allow-list)
//   --root DIR           repo root (default: auto-detected)
//   --no-midi            do not open MIDI inputs

#include "audio/AudioHost.h"
#include "audio/MidiHub.h"
#include "audio/NullAudioDriver.h"
#include "control/ControlServer.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/RtCheck.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

std::atomic<bool> g_quit{false};
extern "C" void onSignal(int) { g_quit.store(true); }

struct Args {
    bool noAudio = false, noMidi = false, help = false;
    int wsPort = 7341, httpPort = 7340;
    std::optional<int> buffer;
    std::optional<double> sampleRate;
    std::string preset, root;
};

Args parseArgs(int argc, char** argv, std::string& err) {
    Args a;
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
            else if (s == "--port") a.wsPort = std::stoi(next());
            else if (s == "--http-port") a.httpPort = std::stoi(next());
            else if (s == "--asio-buffer") a.buffer = std::stoi(next());
            else if (s == "--sample-rate") a.sampleRate = std::stod(next());
            else if (s == "--preset") a.preset = next();
            else if (s == "--root") a.root = next();
            else if (s == "--help" || s == "-h") a.help = true;
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
    Pump(ks::Session& s, ks::ControlServer& srv) : s_(s), srv_(srv) { startTimerHz(30); }
    void timerCallback() override {
        if (g_quit.load()) {
            stopTimer();
            juce::MessageManager::getInstance()->stopDispatchLoop();
            return;
        }
        s_.engine().collectGarbage(); // every 33 ms (ARCHITECTURE: <= 50 ms)
        const auto midi = s_.pollMidi();
        const auto tele = s_.pollTelemetry();
        if (srv_.clientCount() == 0) return;
        if (!midi.is_null()) srv_.broadcast(midi);
        srv_.broadcast(tele);
    }

private:
    ks::Session& s_;
    ks::ControlServer& srv_;
};

void printStatus(const ks::AudioStatus& st, const char* prefix) {
    std::printf("%s device: %s / %s\n", prefix, st.type.c_str(), st.name.c_str());
    std::printf("%s sample rate: %.0f Hz, buffer: %d samples (%.2f ms)\n", prefix, st.sampleRate, st.bufferSize,
                st.sampleRate > 0 ? 1000.0 * st.bufferSize / st.sampleRate : 0.0);
    std::printf("%s latency: input %.2f ms, output %.2f ms (device-reported)\n", prefix, st.inputLatencyMs,
                st.outputLatencyMs);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    std::string err;
    const Args args = parseArgs(argc, argv, err);
    if (!err.empty() || args.help) {
        if (!err.empty()) std::fprintf(stderr, "error: %s\n", err.c_str());
        std::printf("usage: keysynth-engine [--no-audio] [--no-midi] [--port N] [--http-port N] [--asio-buffer N]\n"
                    "                       [--sample-rate HZ] [--preset PATH] [--root DIR]\n");
        return err.empty() ? 0 : 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if defined(SIGBREAK)
    std::signal(SIGBREAK, onSignal);
#endif

    juce::ScopedJuceInitialiser_GUI juceInit; // message manager (no GUI is created)

    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const ks::AppPaths paths = args.root.empty() ? ks::AppPaths::discover(exeDir.getFullPathName().toStdString())
                                                 : ks::AppPaths::fromRoot(args.root);
    std::printf("keysynth-engine — root %s%s\n", ks::pathToUtf8(paths.root).c_str(),
                ks::rt::checksEnabled() ? " (RT checks on)" : "");

    auto engine = std::make_unique<ks::Engine>();
    ks::Session session(*engine, ks::defaultRegistry(), paths);
    ks::ProtocolHandler handler(session);
    ks::ControlServer server(
        handler, [](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); }, paths.uiDist);
    session.log = [&server](const std::string& level, const std::string& msg) {
        std::printf("[%s] %s\n", level.c_str(), msg.c_str());
        std::fflush(stdout);
        server.broadcast({{"type", "log"}, {"level", level}, {"message", msg}});
    };

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
        ks::AudioHost::Options opt;
        opt.bufferSize = args.buffer;
        opt.sampleRate = args.sampleRate;
        const std::string aerr = audioHost->start(opt);
        if (!aerr.empty()) std::fprintf(stderr, "audio: %s\n", aerr.c_str());
        session.setAudio(audioHost.get());
        const auto st = audioHost->status();
        printStatus(st, "audio");
        if (args.buffer && st.bufferSize != *args.buffer)
            std::printf("audio note: requested buffer %d not accepted by the driver (ASIO drivers often only offer the "
                        "size set in their control panel)\n",
                        *args.buffer);
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

    const std::string serr = server.start(args.wsPort, args.httpPort);
    if (!serr.empty()) {
        std::fprintf(stderr, "control server: %s\n", serr.c_str());
        return 1;
    }
    std::printf("control: ws://127.0.0.1:%d%s\n", args.wsPort,
                server.httpRunning() ? (", ui: http://127.0.0.1:" + std::to_string(args.httpPort)).c_str()
                : args.httpPort == 0 ? " (static UI disabled)"
                                     : " (ui/dist not built or port busy: use the Vite dev server)");
    std::printf("ready. Ctrl+C to quit.\n");
    std::fflush(stdout);

    Pump pump(session, server);
    if (audioHost)
        juce::Timer::callAfterDelay(1000, [host = audioHost.get()] {
            std::printf("audio thread: MMCSS \"Pro Audio\" %s (err %lu), power throttling %s\n",
                        host->mmcssActive() ? "on" : "off", host->mmcssError(),
                        host->powerThrottlingDisabled() ? "disabled" : "unchanged");
            std::fflush(stdout);
        });
    juce::MessageManager::getInstance()->runDispatchLoop();

    std::printf("shutting down...\n");
    pump.stopTimer();
    server.stop();
    if (midi) midi->closeAll();
    if (audioHost) audioHost->stop();
    if (nullAudio) nullAudio->stop();
    engine->collectGarbage();
    return 0;
}
