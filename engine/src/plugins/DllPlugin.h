#pragma once
// C ABI plugin DLLs (sdk/include/keysynth/plugin_abi.h, ARCHITECTURE §10).
//  - PluginLibrary: a *copy* of plugins/.build/<name>-<hash>.dll under %TEMP%/keysynth-plugins/<pid>/ (so the
//    build can overwrite/delete the original while it is loaded), LoadLibrary'd, descriptor validated and
//    copied. FreeLibrary + delete the copy in the destructor (control/loader thread; last owner).
//  - DllModule: one plugin instance. Every call into the plugin runs inside an SEH guard with MXCSR saved and
//    restored; a fault mutes the instance for good (the version reports it; reloading creates new instances).

#include "plugins/PluginVersion.h"

#include <keysynth/plugin_abi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ks::plugins {

class PluginLibrary {
public:
    // Copies `dll` into `tempDir` (unique name) and loads it. nullptr + error on failure.
    static std::shared_ptr<PluginLibrary> load(const std::filesystem::path& dll, const std::filesystem::path& tempDir,
                                               std::string& error);
    ~PluginLibrary();
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;

    const ks_plugin_descriptor& descriptor() const noexcept { return desc_; }
    const std::filesystem::path& source() const noexcept { return source_; }

private:
    PluginLibrary() = default;
    void* handle_ = nullptr;
    ks_plugin_descriptor desc_{}; // copy (function pointers into the DLL; strings re-owned by the version)
    std::filesystem::path source_, copy_;
};

class DllPluginVersion final : public PluginVersion {
public:
    // Validates the descriptor and builds ModuleInfo. nullptr + error if invalid.
    static std::shared_ptr<DllPluginVersion> create(const std::string& name, std::shared_ptr<PluginLibrary> lib,
                                                    std::string& error);
    std::unique_ptr<Module> createModule() const override;
    const ks_plugin_descriptor& desc() const noexcept { return lib->descriptor(); }

    std::shared_ptr<PluginLibrary> lib;
};

class DllModule final : public Module {
public:
    explicit DllModule(std::shared_ptr<const DllPluginVersion> v);
    ~DllModule() override;

    bool valid() const noexcept { return inst_ != nullptr; }
    bool faulted() const noexcept { return faulted_; }

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& stereo, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override { return tail_; }
    int latencySamples() const override { return latency_; }
    nlohmann::json saveState() const override;          // {"blob": base64} or null
    void loadState(const nlohmann::json& state) override;

    static std::string base64Encode(const std::vector<unsigned char>& data);
    static bool base64Decode(const std::string& s, std::vector<unsigned char>& out);

private:
    void fault(uint32_t code) noexcept;
    static void guardedProcess(void* self);

    std::shared_ptr<const DllPluginVersion> v_;
    const ks_plugin_descriptor* d_;
    ks_instance inst_ = nullptr;
    std::vector<float> lastSent_;
    bool faulted_ = false;
    int tail_ = 0, latency_ = 0;
    // guardedProcess arguments
    AudioBlock* cur_ = nullptr;
    MidiEventSpan curEvents_;
};

} // namespace ks::plugins
