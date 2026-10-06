#pragma once
// PluginHost (ARCHITECTURE §10, docs/PLUGINS.md): discovers plugins/<name>/ (Faust <name>.dsp, or C++ <name>.cpp
// built by scripts/build_plugin.ps1 into plugins/.build/<name>-<hash>.dll), compiles/loads them on a loader
// thread, registers `plugin:<name>` in the ModuleRegistry and hot-reloads on file changes.
//
// Threads: every public method is control-thread only. The loader thread only compiles/loads and hands
// finished versions back; registry updates and callbacks happen inside poll() / waitIdle() on the control
// thread. Offline mode (async = false) compiles synchronously inside start()/poll().
//
// Security: only direct sub-directories of pluginsDir whose names match [a-z][a-z0-9_]{0,63} are considered
// (no symlinks); reload requests take a plugin *name*, never a path.

#include "core/ModuleRegistry.h"
#include "plugins/PluginVersion.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ks::plugins {

struct PluginStatus {
    std::string name;
    std::string typeId;     // plugin:<name>
    std::string source;     // "faust" | "dll"
    std::string state;      // "compiling" | "ok" | "error" | "faulted" | "removed"
    std::string message;    // error text (Faust compiler output, loader error, fault description)
    std::string moduleKind; // "instrument" | "effect" | "" (unknown until loaded)
    int version = 0;        // loaded version counter (0 = never loaded)
    double compileMs = 0.0;
    bool cached = false;    // Faust machine-code cache hit
};
nlohmann::json toJson(const PluginStatus& s);

class PluginHost {
public:
    struct Options {
        std::filesystem::path pluginsDir;   // <root>/plugins
        std::filesystem::path buildDir;     // default <pluginsDir>/.build (DLLs, cache/)
        std::filesystem::path tempDir;      // default %TEMP%/keysynth-plugins/<pid> (DLL copies)
        bool async = true;                  // loader thread; false = compile inline (offline / tests)
        int scanIntervalMs = 500;           // file watch poll period
    };

    PluginHost(ModuleRegistry& registry, Options opt);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Discover and load plugins. `only`: restrict to these names (empty = all).
    void start(const std::set<std::string>& only = {});
    // Drain finished loads (registry update + callbacks), rescan files every scanIntervalMs, report faults.
    void poll();
    // Rescan now (ignores the interval).
    void rescan();
    // Blocks until nothing is queued or compiling (then drains). False on timeout.
    bool waitIdle(int timeoutMs = 60000);
    // Force a recompile/reload. False + error if unknown.
    bool reload(const std::string& name, std::string& error);

    std::vector<PluginStatus> list() const;
    std::optional<PluginStatus> status(const std::string& name) const;
    std::shared_ptr<const PluginVersion> current(const std::string& name) const;
    const Options& options() const noexcept { return opt_; }

    // Called from poll()/waitIdle() on the control thread.
    std::function<void(const PluginStatus&)> onStatus;
    // Registry entries added/replaced/removed: rebuild graphs using these typeIds.
    std::function<void(const std::vector<std::string>& typeIds)> onModulesChanged;

    static bool isValidName(std::string_view name);
    static nlohmann::json faustInfo(); // {available, version, reason}

private:
    struct Watched {
        PluginSource source = PluginSource::Faust;
        std::filesystem::path dir;
        std::string signature; // files + sizes + mtimes
        std::filesystem::path artifact; // .dsp or newest .dll
        uint64_t requested = 0;  // generation of the newest queued job
    };
    struct Job {
        std::string name;
        PluginSource source;
        std::filesystem::path dir, artifact;
        uint64_t generation = 0;
        int version = 0;
    };
    struct Result {
        Job job;
        std::shared_ptr<PluginVersion> version;
        std::string error;
        double ms = 0.0;
        bool cached = false;
    };

    void scan(bool force);
    void enqueue(const std::string& name, Watched& w);
    Result runJob(const Job& job) const;
    Result loadFaust(const Job& job) const;
    Result loadDll(const Job& job) const;
    void apply(Result r, std::vector<std::string>& changed);
    void drain();
    void checkFaults();
    void setStatus(PluginStatus s);
    void workerLoop();

    ModuleRegistry& registry_;
    Options opt_;
    std::set<std::string> only_;
    std::map<std::string, Watched> watched_;
    std::map<std::string, PluginStatus> status_;
    std::map<std::string, std::shared_ptr<PluginVersion>> current_;
    std::map<std::string, uint32_t> faultsSeen_;
    std::map<std::string, std::chrono::steady_clock::time_point> lastFaultReport_;
    uint64_t nextGeneration_ = 1;
    std::chrono::steady_clock::time_point lastScan_{};

    // loader thread
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Result> done_;
    int busy_ = 0;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace ks::plugins
