#pragma once
// Session: control-thread façade over PatchModel + GraphBuilder + Engine + PresetStore + audio/MIDI hosts.
// Protocol handlers only call Session/PatchModel methods (ARCHITECTURE §5.3). Not thread-safe: message thread.

#include "audio/AudioControl.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/PatchModel.h"
#include "preset/PresetStore.h"
#include "transport/Pattern.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ks::plugins {
class PluginHost;
}

namespace ks {

class Session {
public:
    Session(Engine& engine, const ModuleRegistry& registry, AppPaths paths);

    Engine& engine() noexcept { return engine_; }
    PatchModel& model() noexcept { return model_; }
    const PresetStore& presets() const noexcept { return presets_; }
    const AppPaths& paths() const noexcept { return paths_; }
    const ModuleRegistry& registry() const noexcept { return registry_; }

    void setAudio(AudioControl* a) noexcept { audio_ = a; }
    void setMidi(MidiControl* m) noexcept { midi_ = m; }
    AudioControl* audio() const noexcept { return audio_; }
    MidiControl* midi() const noexcept { return midi_; }
    // Optional plugin host (protocol `list_plugins` / `reload_plugin`).
    void setPlugins(plugins::PluginHost* p) noexcept { plugins_ = p; }
    plugins::PluginHost* plugins() const noexcept { return plugins_; }

    // Optional log sink (ControlServer forwards to clients as `log` events, app prints).
    std::function<void(const std::string& level, const std::string& msg)> log;

    // Build a graph from the PatchModel and publish it. reuse=false after Engine::prepare (new sr/maxBlock).
    void rebuild(bool reuse = true);

    // Registry entries for these typeIds were added/replaced/removed (plugin hot reload): re-normalize the
    // patch params against the new specs (values of surviving ids are kept), carry non-param module state
    // (plugin state blobs) over from the live modules, rebuild. Modules of other types are reused.
    bool refreshModules(const std::vector<std::string>& typeIds); // true if the patch used one of them

    // Whole-patch changes (rebuild included). Return warnings.
    // reuse=false for preset loads (node ids of unrelated patches must not share modules).
    std::vector<std::string> setPatch(Patch p, const std::string& presetPath = {}, bool reuse = true);
    std::vector<std::string> loadPreset(const std::string& path);
    std::string savePreset(const std::string& name, const std::string& category, bool overwrite);

    // Param-like edits: PatchModel first, then the latest published graph's atomics.
    float setParam(NodeId node, const std::string& param, float value);
    void setZone(NodeId layer, const Zone& zone);
    void setFxBypass(NodeId node, bool bypass);
    void setMasterVolume(float db);
    void setTempo(double bpm);

    const std::string& presetPath() const noexcept { return presetPath_; }

    // --- rhythm (drum sequencer, ARCHITECTURE §8). Throw PatchError on bad input. ---
    struct PatternInfo {
        std::string path, name;
        int numerator = 4, denominator = 4, bars = 1;
        double tempo = 0.0;
        bool factory = false;
    };
    std::vector<PatternInfo> listPatterns() const;
    // Load a pattern file and make it current. applyMeta: also apply its tempo and kit (not when it comes with a
    // preset, which defines those itself). "" = empty pattern. Returns warnings.
    std::vector<std::string> loadPattern(const std::string& path, bool applyMeta = true);
    Pattern readPatternFile(const std::string& path, std::vector<std::string>* warnings = nullptr) const;
    // Replace the current pattern (UI step edits). Meter/swing of the pattern become the transport's.
    void setPattern(Pattern p);
    std::string savePattern(const std::string& name, bool overwrite);
    void setTimeSignature(int num, int den);
    void setSwing(float swing);
    const Pattern& pattern() const noexcept { return pattern_; }
    const std::string& patternPath() const noexcept { return patternPath_; }
    bool patternEdited() const noexcept { return patternEdited_; }
    nlohmann::json patternJson() const; // { path, edited, pattern }
    static Pattern emptyPattern();

    // Snapshots for the protocol.
    nlohmann::json stateJson() const;
    nlohmann::json transportJson() const;
    nlohmann::json devicesJson();
    // Poll telemetry (<= 30 Hz). Returns the `telemetry` event.
    nlohmann::json pollTelemetry();
    // Drains note activity; returns a `midi` event or null when nothing happened.
    nlohmann::json pollMidi();
    nlohmann::json transportTelemetry() const; // { ppq, playing, step, bar }
    // Reports new Module::loadError()s of the latest graph's modules once each via `log("error", ...)` (failed sample
    // loads finish asynchronously, so this runs on the 30 Hz pump). Returns the number of new reports.
    int pollModuleErrors();

private:
    void logWarnings(const std::vector<std::string>& w);
    void publishPattern();

    Engine& engine_;
    const ModuleRegistry& registry_;
    AppPaths paths_;
    PatchModel model_;
    PresetStore presets_;
    AudioControl* audio_ = nullptr;
    MidiControl* midi_ = nullptr;
    plugins::PluginHost* plugins_ = nullptr;
    std::string presetPath_;
    Pattern pattern_;
    std::string patternPath_;
    bool patternEdited_ = false;
    std::map<const Module*, std::string> reportedErrors_; // pollModuleErrors: module -> last reported error
};

} // namespace ks
