#pragma once
// Session: control-thread façade over PatchModel + GraphBuilder + Engine + PresetStore + audio/MIDI hosts.
// Protocol handlers only call Session/PatchModel methods (ARCHITECTURE §5.3). Not thread-safe: message thread.

#include "audio/AudioControl.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/PatchModel.h"
#include "preset/PresetStore.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

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

    // Optional log sink (ControlServer forwards to clients as `log` events, app prints).
    std::function<void(const std::string& level, const std::string& msg)> log;

    // Build a graph from the PatchModel and publish it. reuse=false after Engine::prepare (new sr/maxBlock).
    void rebuild(bool reuse = true);

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

    // Snapshots for the protocol.
    nlohmann::json stateJson() const;
    nlohmann::json transportJson() const;
    nlohmann::json devicesJson();
    // Poll telemetry (<= 30 Hz). Returns the `telemetry` event.
    nlohmann::json pollTelemetry();
    // Drains note activity; returns a `midi` event or null when nothing happened.
    nlohmann::json pollMidi();

private:
    void logWarnings(const std::vector<std::string>& w);

    Engine& engine_;
    const ModuleRegistry& registry_;
    AppPaths paths_;
    PatchModel model_;
    PresetStore presets_;
    AudioControl* audio_ = nullptr;
    MidiControl* midi_ = nullptr;
    std::string presetPath_;
};

} // namespace ks
