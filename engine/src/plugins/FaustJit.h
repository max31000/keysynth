#pragma once
// libfaust (LLVM backend) JIT wrapper (ARCHITECTURE §10, docs/PLUGINS.md).
//
// faust.dll is loaded at runtime and only its *C* API is used (llvm-dsp-c.h): the DLL uses the dynamic MSVC
// runtime while keysynth links the static one, so no C++ objects (std::string, ...) may cross the boundary.
// Factory create/read/write/delete are serialized by one mutex (libfaust's compiler is not reentrant; it is
// also put in multi-thread mode). Instance compute/clear are plain JIT calls (RT-safe: Faust's generated
// compute never allocates; the test suite checks the LLVM IR for allocator calls).

#include "plugins/FaustParamMap.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ks::plugins {

// Owns one reference to an llvm_dsp_factory. Destroyed on the control/loader thread only, after all its
// instances (modules hold a shared_ptr through their PluginVersion).
class FaustFactory {
public:
    explicit FaustFactory(void* raw) : raw_(raw) {}
    ~FaustFactory();
    FaustFactory(const FaustFactory&) = delete;
    FaustFactory& operator=(const FaustFactory&) = delete;
    void* raw() const noexcept { return raw_; }

private:
    void* raw_;
};

struct FaustCompileRequest {
    std::filesystem::path dspFile;
    std::vector<std::string> options; // extra (white-listed) Faust options
    std::filesystem::path cacheDir;   // machine-code cache; empty = no cache
    std::string cachePrefix;          // file name prefix (plugin name)
};

struct FaustCompileResult {
    std::shared_ptr<FaustFactory> factory;
    std::string error;                 // compile error (Faust message) when factory is null
    std::vector<std::string> warnings;
    bool cached = false;
    double milliseconds = 0.0;
    FaustUiDesc ui;
    std::map<std::string, std::string> meta; // global declare metadata (name, author, options, ...)
    int numInputs = 0, numOutputs = 0;
    std::string cacheKey;
};

namespace faust {

// Loads faust.dll on first use (search: $KS_FAUST_DIR/lib, the executable's dir, compile-time KS_FAUST_DIR).
bool available();
std::string unavailableReason();
std::string version();
// share/faust directory (stdfaust.lib, ...), passed as -I.
std::filesystem::path libraryDir();

// Control/loader thread. Uses the machine-code cache when the key (source files + options + target +
// libfaust version) matches.
FaustCompileResult compile(const FaustCompileRequest& req);

// LLVM IR text of a factory (tests: no allocator calls in compute).
std::string irText(const FaustFactory& f);

// --- instances (control thread unless noted) ---
void* createInstance(const FaustFactory& f);
void deleteInstance(void* dsp);
void init(void* dsp, int sampleRate);
// Zones (FAUSTFLOAT*) of every control in buildUserInterface order (same order as FaustUiDesc::controls).
std::vector<float*> zones(void* dsp);
// Audio thread (RT-safe).
void clear(void* dsp) noexcept;
void compute(void* dsp, int count, float** inputs, float** outputs) noexcept;

} // namespace faust

} // namespace ks::plugins
