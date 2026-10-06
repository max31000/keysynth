#include "plugins/FaustJit.h"

#include "core/AppPaths.h"
#include "platform/DynamicLibrary.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(KS_HAS_FAUST)
#include <faust/gui/CInterface.h>
static_assert(sizeof(FAUSTFLOAT) == sizeof(float), "keysynth expects FAUSTFLOAT == float");
#endif

namespace fs = std::filesystem;

namespace ks::plugins {

namespace {

#if defined(KS_HAS_FAUST)
struct Api {
    void* lib = nullptr;
    const char* (*getCLibFaustVersion)() = nullptr;
    char* (*getCDSPMachineTarget)() = nullptr;
    void* (*createCDSPFactoryFromFile)(const char*, int, const char*[], const char*, char*, int) = nullptr;
    bool (*deleteCDSPFactory)(void*) = nullptr;
    char* (*writeCDSPFactoryToIR)(void*) = nullptr;
    void* (*readCDSPFactoryFromMachineFile)(const char*, const char*, char*) = nullptr;
    bool (*writeCDSPFactoryToMachineFile)(void*, const char*, const char*) = nullptr;
    bool (*startMTDSPFactories)() = nullptr;
    void (*freeCMemory)(void*) = nullptr;
    void* (*createCDSPInstance)(void*) = nullptr;
    void (*deleteCDSPInstance)(void*) = nullptr;
    int (*getNumInputsCDSPInstance)(void*) = nullptr;
    int (*getNumOutputsCDSPInstance)(void*) = nullptr;
    void (*buildUserInterfaceCDSPInstance)(void*, UIGlue*) = nullptr;
    void (*metadataCDSPInstance)(void*, MetaGlue*) = nullptr;
    void (*initCDSPInstance)(void*, int) = nullptr;
    void (*instanceClearCDSPInstance)(void*) = nullptr;
    void (*computeCDSPInstance)(void*, int, float**, float**) = nullptr;
    std::string error;
    std::string target; // LLVM machine target (cache key)
    fs::path root;      // Faust install dir (share/faust lives under it)
};

std::mutex& compilerMutex() {
    static std::mutex m;
    return m;
}

template <typename F>
bool resolve(Api& a, F& fn, const char* name) {
    fn = reinterpret_cast<F>(platform::findSymbol(a.lib, name));
    if (!fn) a.error = std::string("faust.dll lacks ") + name;
    return fn != nullptr;
}

std::vector<fs::path> faustRootCandidates() {
    std::vector<fs::path> out;
    if (const char* env = std::getenv("KS_FAUST_DIR"); env && *env) out.push_back(pathFromUtf8(env));
#if defined(KS_FAUST_DIR)
    out.push_back(pathFromUtf8(KS_FAUST_DIR));
#endif
    return out;
}

Api& api() {
    static Api a = [] {
        Api r;
        std::string err;
        // 1) <faust>/lib/faust.dll (env or compile-time dir), 2) next to the executable (post-build copy).
        for (const fs::path& root : faustRootCandidates()) {
            std::error_code ec;
            const fs::path dll = root / "lib" / "faust.dll";
            if (!fs::is_regular_file(dll, ec)) continue;
            r.lib = platform::openLibrary(dll, err);
            if (r.lib) {
                r.root = root;
                break;
            }
        }
        if (!r.lib) {
            r.lib = platform::openLibrary("faust.dll", err);
            if (r.lib && !faustRootCandidates().empty()) r.root = faustRootCandidates().front();
        }
        if (!r.lib) {
            r.error = "libfaust (faust.dll) not found; run scripts/fetch_faust.ps1 or set KS_FAUST_DIR";
            return r;
        }
        bool ok = resolve(r, r.getCLibFaustVersion, "getCLibFaustVersion") &&
                  resolve(r, r.getCDSPMachineTarget, "getCDSPMachineTarget") &&
                  resolve(r, r.createCDSPFactoryFromFile, "createCDSPFactoryFromFile") &&
                  resolve(r, r.deleteCDSPFactory, "deleteCDSPFactory") &&
                  resolve(r, r.writeCDSPFactoryToIR, "writeCDSPFactoryToIR") &&
                  resolve(r, r.readCDSPFactoryFromMachineFile, "readCDSPFactoryFromMachineFile") &&
                  resolve(r, r.writeCDSPFactoryToMachineFile, "writeCDSPFactoryToMachineFile") &&
                  resolve(r, r.startMTDSPFactories, "startMTDSPFactories") &&
                  resolve(r, r.freeCMemory, "freeCMemory") &&
                  resolve(r, r.createCDSPInstance, "createCDSPInstance") &&
                  resolve(r, r.deleteCDSPInstance, "deleteCDSPInstance") &&
                  resolve(r, r.getNumInputsCDSPInstance, "getNumInputsCDSPInstance") &&
                  resolve(r, r.getNumOutputsCDSPInstance, "getNumOutputsCDSPInstance") &&
                  resolve(r, r.buildUserInterfaceCDSPInstance, "buildUserInterfaceCDSPInstance") &&
                  resolve(r, r.metadataCDSPInstance, "metadataCDSPInstance") &&
                  resolve(r, r.initCDSPInstance, "initCDSPInstance") &&
                  resolve(r, r.instanceClearCDSPInstance, "instanceClearCDSPInstance") &&
                  resolve(r, r.computeCDSPInstance, "computeCDSPInstance");
        if (!ok) {
            platform::closeLibrary(r.lib);
            r.lib = nullptr;
            return r;
        }
        r.startMTDSPFactories();
        if (char* t = r.getCDSPMachineTarget()) {
            r.target = t;
            r.freeCMemory(t);
        }
        return r;
    }();
    return a;
}

// --- UI introspection -------------------------------------------------------------------------------------

struct UiCollector {
    FaustUiDesc desc;
    std::vector<std::string> stack; // open groups (index 0 = root)
    std::map<float*, std::map<std::string, std::string>> pending;

    void open(const char* label) { stack.emplace_back(label ? label : ""); }
    void close() {
        if (!stack.empty()) stack.pop_back();
    }
    void add(FaustControl::Kind k, const char* label, float* zone, float init, float mn, float mx, float step) {
        FaustControl c;
        c.kind = k;
        c.label = label ? label : "";
        if (stack.size() > 1) c.groups.assign(stack.begin() + 1, stack.end());
        c.init = init;
        c.min = mn;
        c.max = mx;
        c.step = step;
        if (auto it = pending.find(zone); it != pending.end()) {
            c.meta = std::move(it->second);
            pending.erase(it);
        }
        desc.controls.push_back(std::move(c));
    }
};

UiCollector* uc(void* p) { return static_cast<UiCollector*>(p); }

UIGlue makeDescGlue(UiCollector& c) {
    using K = FaustControl::Kind;
    UIGlue g{};
    g.uiInterface = &c;
    g.openTabBox = [](void* u, const char* l) { uc(u)->open(l); };
    g.openHorizontalBox = [](void* u, const char* l) { uc(u)->open(l); };
    g.openVerticalBox = [](void* u, const char* l) { uc(u)->open(l); };
    g.closeBox = [](void* u) { uc(u)->close(); };
    g.addButton = [](void* u, const char* l, float* z) { uc(u)->add(K::Button, l, z, 0, 0, 1, 1); };
    g.addCheckButton = [](void* u, const char* l, float* z) { uc(u)->add(K::CheckBox, l, z, 0, 0, 1, 1); };
    g.addVerticalSlider = [](void* u, const char* l, float* z, float i, float mn, float mx, float s) {
        uc(u)->add(K::VSlider, l, z, i, mn, mx, s);
    };
    g.addHorizontalSlider = [](void* u, const char* l, float* z, float i, float mn, float mx, float s) {
        uc(u)->add(K::HSlider, l, z, i, mn, mx, s);
    };
    g.addNumEntry = [](void* u, const char* l, float* z, float i, float mn, float mx, float s) {
        uc(u)->add(K::NumEntry, l, z, i, mn, mx, s);
    };
    g.addHorizontalBargraph = [](void* u, const char* l, float* z, float mn, float mx) {
        uc(u)->add(K::HBargraph, l, z, mn, mn, mx, 0);
    };
    g.addVerticalBargraph = [](void* u, const char* l, float* z, float mn, float mx) {
        uc(u)->add(K::VBargraph, l, z, mn, mn, mx, 0);
    };
    g.addSoundfile = [](void* u, const char*, const char*, Soundfile**) { uc(u)->desc.hasSoundfile = true; };
    g.declare = [](void* u, float* z, const char* k, const char* v) {
        if (z && k) uc(u)->pending[z][k] = v ? v : "";
    };
    return g;
}

UIGlue makeZoneGlue(std::vector<float*>& zones) {
    UIGlue g{};
    g.uiInterface = &zones;
    g.openTabBox = [](void*, const char*) {};
    g.openHorizontalBox = [](void*, const char*) {};
    g.openVerticalBox = [](void*, const char*) {};
    g.closeBox = [](void*) {};
    g.addButton = [](void* u, const char*, float* z) { static_cast<std::vector<float*>*>(u)->push_back(z); };
    g.addCheckButton = [](void* u, const char*, float* z) { static_cast<std::vector<float*>*>(u)->push_back(z); };
    g.addVerticalSlider = [](void* u, const char*, float* z, float, float, float, float) {
        static_cast<std::vector<float*>*>(u)->push_back(z);
    };
    g.addHorizontalSlider = [](void* u, const char*, float* z, float, float, float, float) {
        static_cast<std::vector<float*>*>(u)->push_back(z);
    };
    g.addNumEntry = [](void* u, const char*, float* z, float, float, float, float) {
        static_cast<std::vector<float*>*>(u)->push_back(z);
    };
    g.addHorizontalBargraph = [](void* u, const char*, float* z, float, float) {
        static_cast<std::vector<float*>*>(u)->push_back(z);
    };
    g.addVerticalBargraph = [](void* u, const char*, float* z, float, float) {
        static_cast<std::vector<float*>*>(u)->push_back(z);
    };
    g.addSoundfile = [](void*, const char*, const char*, Soundfile**) {};
    g.declare = [](void*, float*, const char*, const char*) {};
    return g;
}

// --- cache key ---------------------------------------------------------------------------------------------

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    void add(const std::string& s) {
        add(s.data(), s.size());
        const char sep = 0;
        add(&sep, 1);
    }
};

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string cacheKey(const FaustCompileRequest& req, const Api& a) {
    Fnv f;
    f.add(std::string(a.getCLibFaustVersion ? a.getCLibFaustVersion() : ""));
    f.add(a.target);
    for (const auto& o : req.options) f.add(o);
    // Every Faust source next to the plugin (local imports); stdlib changes come with a new libfaust version.
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(req.dspFile.parent_path(), ec)) {
        const auto ext = e.path().extension().string();
        if (e.is_regular_file(ec) && (ext == ".dsp" || ext == ".lib")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        f.add(pathToUtf8(p.filename()));
        f.add(readFile(p));
    }
    f.add(pathToUtf8(req.dspFile.filename()));
    return hex64(f.h);
}

void pruneCache(const fs::path& dir, const std::string& prefix, const fs::path& keep) {
    std::vector<std::pair<fs::file_time_type, fs::path>> old;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string fn = pathToUtf8(e.path().filename());
        if (e.path() == keep || fn.rfind(prefix + "-", 0) != 0 || e.path().extension() != ".fmc") continue;
        old.emplace_back(e.last_write_time(ec), e.path());
    }
    std::sort(old.begin(), old.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t i = 3; i < old.size(); ++i) fs::remove(old[i].second, ec); // keep a few (quick undo)
}

void describe(void* factory, FaustCompileResult& r) {
    Api& a = api();
    void* dsp = a.createCDSPInstance(factory);
    if (!dsp) {
        r.error = "createCDSPInstance failed";
        return;
    }
    r.numInputs = a.getNumInputsCDSPInstance(dsp);
    r.numOutputs = a.getNumOutputsCDSPInstance(dsp);
    UiCollector c;
    UIGlue g = makeDescGlue(c);
    a.buildUserInterfaceCDSPInstance(dsp, &g);
    r.ui = std::move(c.desc);
    MetaGlue m{};
    m.metaInterface = &r.meta;
    m.declare = [](void* u, const char* k, const char* v) {
        if (k) (*static_cast<std::map<std::string, std::string>*>(u))[k] = v ? v : "";
    };
    a.metadataCDSPInstance(dsp, &m);
    a.deleteCDSPInstance(dsp);
}
#endif

} // namespace

FaustFactory::~FaustFactory() {
#if defined(KS_HAS_FAUST)
    if (raw_ && api().lib) {
        std::lock_guard<std::mutex> lk(compilerMutex());
        api().deleteCDSPFactory(raw_);
    }
#endif
}

namespace faust {

#if defined(KS_HAS_FAUST)

bool available() { return api().lib != nullptr; }
std::string unavailableReason() { return api().error; }
std::string version() { return available() ? api().getCLibFaustVersion() : std::string(); }

fs::path libraryDir() {
    if (!api().root.empty()) return api().root / "share" / "faust";
    return {};
}

FaustCompileResult compile(const FaustCompileRequest& req) {
    FaustCompileResult r;
    if (!available()) {
        r.error = unavailableReason();
        return r;
    }
    Api& a = api();
    const auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    if (!fs::is_regular_file(req.dspFile, ec)) {
        r.error = "source not found: " + pathToUtf8(req.dspFile);
        return r;
    }
    r.cacheKey = cacheKey(req, a);
    fs::path cacheFile;
    if (!req.cacheDir.empty()) {
        fs::create_directories(req.cacheDir, ec);
        cacheFile = req.cacheDir / (req.cachePrefix + "-" + r.cacheKey + ".fmc");
    }

    std::lock_guard<std::mutex> lk(compilerMutex());
    char err[4096] = {0};
    void* factory = nullptr;
    if (!cacheFile.empty() && fs::is_regular_file(cacheFile, ec)) {
        factory = a.readCDSPFactoryFromMachineFile(pathToUtf8(cacheFile).c_str(), "", err);
        if (factory) r.cached = true;
        else fs::remove(cacheFile, ec); // stale/corrupt: recompile below
    }
    if (!factory) {
        err[0] = 0;
        const std::string lib = pathToUtf8(libraryDir());
        const std::string dir = pathToUtf8(req.dspFile.parent_path());
        std::vector<std::string> args;
        if (!lib.empty()) {
            args.push_back("-I");
            args.push_back(lib);
        }
        args.push_back("-I");
        args.push_back(dir);
        for (const auto& o : req.options) args.push_back(o);
        std::vector<const char*> argv;
        for (const auto& s : args) argv.push_back(s.c_str());
        argv.push_back(nullptr);
        const std::string file = pathToUtf8(req.dspFile);
        factory = a.createCDSPFactoryFromFile(file.c_str(), static_cast<int>(args.size()), argv.data(), "", err, -1);
        if (!factory) {
            r.error = err[0] ? std::string(err) : std::string("Faust compilation failed");
            while (!r.error.empty() && (r.error.back() == '\n' || r.error.back() == '\r')) r.error.pop_back();
            return r;
        }
        if (!cacheFile.empty()) {
            // Write to a temp name, then rename: a concurrent reader never sees a partial file.
            const fs::path tmp = cacheFile.string() + ".tmp";
            if (a.writeCDSPFactoryToMachineFile(factory, pathToUtf8(tmp).c_str(), "")) {
                fs::rename(tmp, cacheFile, ec);
                if (ec) fs::remove(tmp, ec);
                pruneCache(req.cacheDir, req.cachePrefix, cacheFile);
            }
        }
    }
    r.factory = std::make_shared<FaustFactory>(factory);
    describe(factory, r);
    if (!r.error.empty()) r.factory.reset();
    r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

std::string irText(const FaustFactory& f) {
    if (!available() || !f.raw()) return {};
    std::lock_guard<std::mutex> lk(compilerMutex());
    char* s = api().writeCDSPFactoryToIR(f.raw());
    if (!s) return {};
    std::string out = s;
    api().freeCMemory(s);
    return out;
}

void* createInstance(const FaustFactory& f) { return available() && f.raw() ? api().createCDSPInstance(f.raw()) : nullptr; }
void deleteInstance(void* dsp) {
    if (dsp) api().deleteCDSPInstance(dsp);
}
void init(void* dsp, int sampleRate) { api().initCDSPInstance(dsp, sampleRate); }

std::vector<float*> zones(void* dsp) {
    std::vector<float*> z;
    UIGlue g = makeZoneGlue(z);
    api().buildUserInterfaceCDSPInstance(dsp, &g);
    return z;
}

void clear(void* dsp) noexcept { api().instanceClearCDSPInstance(dsp); }
void compute(void* dsp, int count, float** inputs, float** outputs) noexcept {
    api().computeCDSPInstance(dsp, count, inputs, outputs);
}

#else // !KS_HAS_FAUST

bool available() { return false; }
std::string unavailableReason() { return "keysynth was built without Faust (KS_WITH_FAUST=OFF or Faust not found)"; }
std::string version() { return {}; }
fs::path libraryDir() { return {}; }
FaustCompileResult compile(const FaustCompileRequest&) {
    FaustCompileResult r;
    r.error = unavailableReason();
    return r;
}
std::string irText(const FaustFactory&) { return {}; }
void* createInstance(const FaustFactory&) { return nullptr; }
void deleteInstance(void*) {}
void init(void*, int) {}
std::vector<float*> zones(void*) { return {}; }
void clear(void*) noexcept {}
void compute(void*, int, float**, float**) noexcept {}

#endif

} // namespace faust

} // namespace ks::plugins
