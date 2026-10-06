#include "plugins/PluginHost.h"

#include "core/AppPaths.h"
#include "plugins/DllPlugin.h"
#include "plugins/FaustJit.h"
#include "plugins/FaustModule.h"
#include "platform/DynamicLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ks::plugins {

namespace {

constexpr int kMaxPolyphony = FaustInstrument::kMaxVoices;

struct Manifest {
    std::string name, category, kind; // kind: "", "instrument", "effect"
    int polyphony = 0;                // 0 = from Faust [nvoices:N] metadata or 8
    float maxReleaseSeconds = 10.0f;
    float tailSeconds = 1.0f;
    std::vector<std::string> faustOptions;
};

std::string hexCode(uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", c);
    return b;
}

// White-listed Faust compiler options (anything touching files, FAUSTFLOAT or the backend is refused).
bool checkFaustOptions(const std::vector<std::string>& opts, std::string& error) {
    static const std::set<std::string> flags = {"-vec", "-scal", "-dfs", "-fun", "-exp10", "-mapp"};
    static const std::set<std::string> withInt = {"-vs", "-lv", "-mcd", "-ftz", "-fm"};
    for (size_t i = 0; i < opts.size(); ++i) {
        if (flags.count(opts[i])) continue;
        if (withInt.count(opts[i]) && i + 1 < opts.size() && !opts[i + 1].empty() &&
            opts[i + 1].find_first_not_of("0123456789") == std::string::npos && opts[i + 1].size() <= 6) {
            ++i;
            continue;
        }
        error = "faust_options: '" + opts[i] + "' is not allowed (allowed: -vec -scal -dfs -fun -exp10 -mapp, "
                "-vs/-lv/-mcd/-ftz/-fm N)";
        return false;
    }
    return true;
}

bool readManifest(const fs::path& dir, const std::string& name, Manifest& m, std::string& error) {
    const fs::path p = dir / "plugin.json";
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return true; // optional
    std::ifstream f(p, std::ios::binary);
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "plugin.json: invalid JSON";
        return false;
    }
    auto str = [&](const char* k, std::string& out) {
        if (auto it = j.find(k); it != j.end() && it->is_string()) out = it->get<std::string>();
    };
    std::string id;
    str("id", id);
    if (!id.empty() && id != name) {
        error = "plugin.json: id '" + id + "' must equal the directory name '" + name + "'";
        return false;
    }
    str("name", m.name);
    str("category", m.category);
    str("kind", m.kind);
    if (!m.kind.empty() && m.kind != "instrument" && m.kind != "effect") {
        error = "plugin.json: kind must be \"instrument\" or \"effect\"";
        return false;
    }
    if (auto it = j.find("polyphony"); it != j.end()) {
        if (!it->is_number_integer() || it->get<int>() < 1 || it->get<int>() > kMaxPolyphony) {
            error = "plugin.json: polyphony must be 1.." + std::to_string(kMaxPolyphony);
            return false;
        }
        m.polyphony = it->get<int>();
    }
    auto num = [&](const char* k, float& out, float lo, float hi) {
        if (auto it = j.find(k); it != j.end()) {
            if (!it->is_number() || it->get<double>() < lo || it->get<double>() > hi) {
                error = std::string("plugin.json: ") + k + " must be a number in " + std::to_string(lo) + ".." +
                        std::to_string(hi);
                return false;
            }
            out = it->get<float>();
        }
        return true;
    };
    if (!num("max_release_s", m.maxReleaseSeconds, 0.05f, 60.0f) || !num("tail_s", m.tailSeconds, 0.0f, 30.0f))
        return false;
    if (auto it = j.find("faust_options"); it != j.end()) {
        if (!it->is_array()) {
            error = "plugin.json: faust_options must be an array of strings";
            return false;
        }
        for (const auto& o : *it) {
            if (!o.is_string()) {
                error = "plugin.json: faust_options must be an array of strings";
                return false;
            }
            m.faustOptions.push_back(o.get<std::string>());
        }
        if (!checkFaustOptions(m.faustOptions, error)) return false;
    }
    return true;
}

std::string fileSig(const fs::path& p) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec) return {};
    const auto t = fs::last_write_time(p, ec);
    return pathToUtf8(p.filename()) + ":" + std::to_string(sz) + ":" +
           std::to_string(static_cast<long long>(t.time_since_epoch().count())) + ";";
}

bool isHex(std::string_view s) {
    return !s.empty() && s.size() <= 32 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}

// Newest plugins/.build/<name>-<hex>.dll.
fs::path newestDll(const fs::path& buildDir, const std::string& name) {
    fs::path best;
    fs::file_time_type bestT{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(buildDir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".dll") continue;
        const std::string stem = pathToUtf8(e.path().stem());
        if (stem.size() <= name.size() + 1 || stem.compare(0, name.size() + 1, name + "-") != 0) continue;
        if (!isHex(std::string_view(stem).substr(name.size() + 1))) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestT) {
            best = e.path();
            bestT = t;
        }
    }
    return best;
}

int nvoicesFromMeta(const std::map<std::string, std::string>& meta) {
    auto it = meta.find("options");
    if (it == meta.end()) return 0;
    const auto pos = it->second.find("[nvoices:");
    if (pos == std::string::npos) return 0;
    try {
        return std::stoi(it->second.substr(pos + 9));
    } catch (...) {
        return 0;
    }
}

} // namespace

json toJson(const PluginStatus& s) {
    return {{"name", s.name},       {"typeId", s.typeId},         {"source", s.source},
            {"state", s.state},     {"message", s.message},       {"kind", s.moduleKind},
            {"version", s.version}, {"compileMs", std::round(s.compileMs * 10.0) / 10.0}, {"cached", s.cached}};
}

bool PluginHost::isValidName(std::string_view n) {
    if (n.empty() || n.size() > 64 || !(n[0] >= 'a' && n[0] <= 'z')) return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

json PluginHost::faustInfo() {
    const bool ok = faust::available();
    return {{"available", ok}, {"version", faust::version()}, {"reason", ok ? std::string() : faust::unavailableReason()}};
}

PluginHost::PluginHost(ModuleRegistry& registry, Options opt) : registry_(registry), opt_(std::move(opt)) {
    if (opt_.buildDir.empty()) opt_.buildDir = opt_.pluginsDir / ".build";
    std::error_code ec;
    const fs::path tempRoot = fs::temp_directory_path(ec) / "keysynth-plugins";
    if (opt_.tempDir.empty()) {
        // Leftovers of crashed runs: remove what is not locked by a live process.
        for (const auto& e : fs::directory_iterator(tempRoot, ec)) fs::remove_all(e.path(), ec);
        opt_.tempDir = tempRoot / std::to_string(platform::currentProcessId());
    }
    if (opt_.async) worker_ = std::thread([this] { workerLoop(); });
}

PluginHost::~PluginHost() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    done_.clear();
    // Drop our registry entries (modules still alive in graphs keep their version/library).
    for (const auto& [name, v] : current_) registry_.remove(v->info.typeId);
    current_.clear();
    std::error_code ec;
    fs::remove(opt_.tempDir, ec); // only if empty
}

void PluginHost::start(const std::set<std::string>& only) {
    only_ = only;
    scan(true);
    if (!opt_.async) drain();
}

void PluginHost::poll() {
    drain();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastScan_ >= std::chrono::milliseconds(opt_.scanIntervalMs)) {
        scan(false);
        if (!opt_.async) drain();
    }
    checkFaults();
}

void PluginHost::rescan() {
    scan(false);
    if (!opt_.async) drain();
}

bool PluginHost::waitIdle(int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        drain();
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (jobs_.empty() && busy_ == 0 && done_.empty()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            cv_.wait_for(lk, std::chrono::milliseconds(10));
        }
    }
}

bool PluginHost::reload(const std::string& name, std::string& error) {
    if (!isValidName(name)) {
        error = "invalid plugin name";
        return false;
    }
    scan(false);
    auto it = watched_.find(name);
    if (it == watched_.end()) {
        error = "unknown plugin '" + name + "'";
        return false;
    }
    if (it->second.artifact.empty()) {
        error = "plugin '" + name + "' has nothing to load" +
                (it->second.source == PluginSource::Dll ? " (run scripts/build_plugin.ps1 " + name + ")" : "");
        return false;
    }
    enqueue(name, it->second);
    if (!opt_.async) drain();
    return true;
}

std::vector<PluginStatus> PluginHost::list() const {
    std::vector<PluginStatus> out;
    for (const auto& [n, s] : status_) out.push_back(s);
    return out;
}

std::optional<PluginStatus> PluginHost::status(const std::string& name) const {
    auto it = status_.find(name);
    if (it == status_.end()) return std::nullopt;
    return it->second;
}

std::shared_ptr<const PluginVersion> PluginHost::current(const std::string& name) const {
    auto it = current_.find(name);
    return it == current_.end() ? nullptr : it->second;
}

void PluginHost::setStatus(PluginStatus s) {
    status_[s.name] = s;
    if (onStatus) onStatus(s);
}

void PluginHost::scan(bool force) {
    lastScan_ = std::chrono::steady_clock::now();
    std::set<std::string> seen;
    std::error_code ec;
    if (fs::is_directory(opt_.pluginsDir, ec)) {
        for (const auto& e : fs::directory_iterator(opt_.pluginsDir, ec)) {
            if (!e.is_directory(ec) || e.is_symlink(ec)) continue;
            const std::string name = pathToUtf8(e.path().filename());
            if (!isValidName(name)) continue; // also skips .build
            if (!only_.empty() && !only_.count(name)) continue;
            const fs::path dsp = e.path() / (name + ".dsp");
            const fs::path cpp = e.path() / (name + ".cpp");
            Watched w;
            w.dir = e.path();
            if (fs::is_regular_file(dsp, ec)) {
                w.source = PluginSource::Faust;
                w.artifact = dsp;
                // Signature over every Faust source (local imports) + the manifest.
                std::vector<fs::path> files;
                for (const auto& f : fs::directory_iterator(e.path(), ec)) {
                    const auto ext = f.path().extension();
                    if (f.is_regular_file(ec) && (ext == ".dsp" || ext == ".lib" || f.path().filename() == "plugin.json"))
                        files.push_back(f.path());
                }
                std::sort(files.begin(), files.end());
                for (const auto& f : files) w.signature += fileSig(f);
            } else if (fs::is_regular_file(cpp, ec)) {
                w.source = PluginSource::Dll;
                w.artifact = newestDll(opt_.buildDir, name);
                w.signature = w.artifact.empty() ? std::string("unbuilt") : fileSig(w.artifact) + pathToUtf8(w.artifact);
            } else {
                continue;
            }
            seen.insert(name);
            auto it = watched_.find(name);
            const bool isNew = it == watched_.end();
            if (!isNew && !force && it->second.signature == w.signature) continue;
            if (isNew) it = watched_.emplace(name, std::move(w)).first;
            else {
                const uint64_t req = it->second.requested;
                it->second = std::move(w);
                it->second.requested = req;
            }
            Watched& cur = it->second;
            if (cur.artifact.empty()) {
                PluginStatus s;
                s.name = name;
                s.typeId = "plugin:" + name;
                s.source = "dll";
                s.state = "error";
                s.message = "not built: run scripts/build_plugin.ps1 " + name;
                if (auto st = status_.find(name); st != status_.end()) {
                    s.version = st->second.version;
                    s.moduleKind = st->second.moduleKind;
                }
                setStatus(s);
                continue;
            }
            enqueue(name, cur);
        }
    }
    // Removed plugins.
    std::vector<std::string> changed;
    for (auto it = watched_.begin(); it != watched_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        const std::string name = it->first;
        it = watched_.erase(it);
        if (auto c = current_.find(name); c != current_.end()) {
            registry_.remove(c->second->info.typeId);
            changed.push_back(c->second->info.typeId);
            current_.erase(c);
        }
        PluginStatus s = status_.count(name) ? status_[name] : PluginStatus{};
        s.name = name;
        s.typeId = "plugin:" + name;
        s.state = "removed";
        s.message = {};
        setStatus(s);
        status_.erase(name);
    }
    if (!changed.empty() && onModulesChanged) onModulesChanged(changed);
}

void PluginHost::enqueue(const std::string& name, Watched& w) {
    Job job;
    job.name = name;
    job.source = w.source;
    job.dir = w.dir;
    job.artifact = w.artifact;
    job.generation = nextGeneration_++;
    w.requested = job.generation;

    PluginStatus s;
    if (auto it = status_.find(name); it != status_.end()) s = it->second;
    s.name = name;
    s.typeId = "plugin:" + name;
    s.source = w.source == PluginSource::Faust ? "faust" : "dll";
    s.state = "compiling";
    s.message = {};
    s.compileMs = 0.0;
    s.cached = false;
    setStatus(s);

    if (!opt_.async) {
        Result r = runJob(job);
        std::lock_guard<std::mutex> lk(mutex_);
        done_.push_back(std::move(r));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // Coalesce: a queued (not yet started) job for the same plugin is superseded.
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.name == name; }), jobs_.end());
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void PluginHost::workerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++busy_;
        }
        Result r = runJob(job);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            --busy_;
            if (!stop_) done_.push_back(std::move(r));
        }
        cv_.notify_all();
    }
}

PluginHost::Result PluginHost::runJob(const Job& job) const {
    try {
        return job.source == PluginSource::Faust ? loadFaust(job) : loadDll(job);
    } catch (const std::exception& e) {
        Result r;
        r.job = job;
        r.error = std::string("internal error: ") + e.what();
        return r;
    }
}

PluginHost::Result PluginHost::loadFaust(const Job& job) const {
    Result r;
    r.job = job;
    Manifest man;
    if (!readManifest(job.dir, job.name, man, r.error)) return r;
    if (!faust::available()) {
        r.error = faust::unavailableReason();
        return r;
    }
    {
        // Faust rejects a UTF-8 BOM with a cryptic "unexpected EXTRA"; say what is wrong.
        std::ifstream f(job.artifact, std::ios::binary);
        char bom[3] = {0, 0, 0};
        f.read(bom, 3);
        if (f.gcount() == 3 && bom[0] == '\xEF' && bom[1] == '\xBB' && bom[2] == '\xBF') {
            r.error = pathToUtf8(job.artifact.filename()) + " starts with a UTF-8 BOM; save it as UTF-8 without BOM";
            return r;
        }
    }
    FaustCompileRequest req;
    req.dspFile = job.artifact;
    req.options = man.faustOptions;
    req.cacheDir = opt_.buildDir / "cache";
    req.cachePrefix = job.name;
    FaustCompileResult c = faust::compile(req);
    r.ms = c.milliseconds;
    r.cached = c.cached;
    if (!c.factory) {
        r.error = c.error;
        return r;
    }
    bool instrument = man.kind == "instrument";
    if (man.kind.empty()) {
        bool hasGate = false;
        for (const auto& ctl : c.ui.controls) hasGate |= toSnakeCase(ctl.label) == "gate";
        instrument = c.numInputs == 0 && hasGate;
    }
    if (instrument && (c.numInputs != 0 || c.numOutputs < 1 || c.numOutputs > 2)) {
        r.error = "instrument must have 0 inputs and 1-2 outputs (has " + std::to_string(c.numInputs) + "/" +
                  std::to_string(c.numOutputs) + ")";
        return r;
    }
    if (!instrument && (c.numInputs < 1 || c.numInputs > 2 || c.numOutputs < 1 || c.numOutputs > 2)) {
        r.error = "effect must have 1-2 inputs and 1-2 outputs (has " + std::to_string(c.numInputs) + "/" +
                  std::to_string(c.numOutputs) + ")";
        return r;
    }
    FaustMapping mapping = mapFaustUi(c.ui, instrument);
    if (!mapping.error.empty()) {
        r.error = mapping.error;
        return r;
    }
    auto v = std::make_shared<FaustPluginVersion>();
    v->name = job.name;
    v->source = PluginSource::Faust;
    v->factory = c.factory;
    v->numInputs = c.numInputs;
    v->numOutputs = c.numOutputs;
    v->instrument = instrument;
    v->polyphony = man.polyphony > 0 ? man.polyphony : std::clamp(nvoicesFromMeta(c.meta), 0, kMaxPolyphony);
    if (v->polyphony <= 0) v->polyphony = 8;
    v->maxReleaseSeconds = man.maxReleaseSeconds;
    v->tailSeconds = man.tailSeconds;
    ModuleInfo& info = v->info;
    info.typeId = "plugin:" + job.name;
    info.displayName = !man.name.empty() ? man.name : (c.meta.count("name") ? c.meta["name"] : job.name);
    info.kind = instrument ? ModuleKind::Instrument : ModuleKind::Effect;
    info.category = !man.category.empty() ? man.category : "Plugin";
    info.params = mapping.params;
    if (!mapping.groupOrder.empty()) info.uiHints["groupOrder"] = mapping.groupOrder;
    v->mapping = std::move(mapping);
    r.version = std::move(v);
    return r;
}

PluginHost::Result PluginHost::loadDll(const Job& job) const {
    Result r;
    r.job = job;
    const auto t0 = std::chrono::steady_clock::now();
    auto lib = PluginLibrary::load(job.artifact, opt_.tempDir, r.error);
    if (!lib) return r;
    auto v = DllPluginVersion::create(job.name, std::move(lib), r.error);
    if (!v) return r;
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.version = std::move(v);
    return r;
}

void PluginHost::drain() {
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        done.swap(done_);
    }
    std::vector<std::string> changed;
    for (auto& r : done) apply(std::move(r), changed);
    if (!changed.empty() && onModulesChanged) onModulesChanged(changed);
}

void PluginHost::apply(Result r, std::vector<std::string>& changed) {
    const std::string& name = r.job.name;
    auto w = watched_.find(name);
    // Superseded (a newer job is queued) or removed meanwhile: drop; the version is released here.
    if (w == watched_.end() || w->second.requested != r.job.generation) return;
    PluginStatus s = status_.count(name) ? status_[name] : PluginStatus{};
    s.name = name;
    s.typeId = "plugin:" + name;
    s.source = r.job.source == PluginSource::Faust ? "faust" : "dll";
    s.compileMs = r.ms;
    s.cached = r.cached;
    if (!r.version) {
        // Keep the previous good version registered (if any): the patch keeps sounding.
        s.state = "error";
        s.message = r.error;
        setStatus(s);
        return;
    }
    r.version->version = s.version + 1;
    std::shared_ptr<PluginVersion> v = r.version;
    registry_.add(v->info, [v]() { return v->createModule(); });
    current_[name] = v;
    faultsSeen_[name] = 0;
    s.version = v->version;
    s.state = "ok";
    s.message = {};
    s.moduleKind = toString(v->info.kind);
    changed.push_back(v->info.typeId);
    setStatus(s);
}

void PluginHost::checkFaults() {
    for (const auto& [name, v] : current_) {
        const uint32_t n = v->faults.load(std::memory_order_acquire);
        uint32_t& seen = faultsSeen_[name];
        if (n == seen) continue;
        seen = n;
        PluginStatus s = status_[name];
        const uint32_t code = v->faultCode.load(std::memory_order_relaxed);
        s.state = "faulted";
        s.message = code == kFaultNonFinite
                        ? "produced NaN/Inf output (block zeroed, state cleared)"
                        : "crashed (exception " + hexCode(code) + "); instance muted until the plugin is reloaded";
        setStatus(s);
    }
}

} // namespace ks::plugins
