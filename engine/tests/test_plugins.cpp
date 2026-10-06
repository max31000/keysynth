// Plugin host tests (ARCHITECTURE §10, docs/PLUGINS.md): Faust JIT (compile, param mapping, poly instrument,
// effects, hot reload, errors, RT), C ABI DLLs (build via scripts/build_plugin.ps1, load/reload/unload, SEH).
// Faust tests SKIP when libfaust is unavailable; DLL tests SKIP when MSVC is not installed.

#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/GraphBuilder.h"
#include "core/ModuleRegistry.h"
#include "core/RtCheck.h"
#include "plugins/DllPlugin.h"
#include "plugins/FaustJit.h"
#include "plugins/FaustModule.h"
#include "plugins/FaustParamMap.h"
#include "plugins/PluginHost.h"
#include "render/OfflineRenderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace ks;
using namespace ks::plugins;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& tag) {
    static std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path p = fs::temp_directory_path() / ("ks-plugins-" + tag + "-" + std::to_string(rng()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// Make sure the watcher sees a change even on coarse-mtime file systems.
void bumpMtime(const fs::path& p, int seconds) {
    fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(seconds));
}

void requireFaust() {
    if (!faust::available()) SKIP("Faust not available: " + faust::unavailableReason());
}

struct TestHost {
    fs::path root, plugins;
    ModuleRegistry registry;
    std::unique_ptr<PluginHost> host;
    std::vector<PluginStatus> events;
    std::vector<std::string> changed;

    explicit TestHost(const std::string& tag) : root(tempDir(tag)), plugins(root / "plugins") {
        fs::create_directories(plugins);
        registerBuiltinModules(registry);
    }
    void start(bool async = false) {
        PluginHost::Options o;
        o.pluginsDir = plugins;
        o.async = async;
        o.tempDir = root / "tmp";
        host = std::make_unique<PluginHost>(registry, o);
        host->onStatus = [this](const PluginStatus& s) { events.push_back(s); };
        host->onModulesChanged = [this](const std::vector<std::string>& ids) {
            changed.insert(changed.end(), ids.begin(), ids.end());
        };
        host->start();
    }
    ~TestHost() {
        host.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    PluginStatus st(const std::string& name) const {
        auto s = host->status(name);
        REQUIRE(s.has_value());
        return *s;
    }
};

const ParamSpec* findParam(const ModuleInfo& info, const std::string& id) {
    for (const auto& p : info.params)
        if (p.id == id) return &p;
    return nullptr;
}

double zeroCrossingHz(const std::vector<float>& x, double sr, double t0, double t1) {
    const size_t a = static_cast<size_t>(t0 * sr), b = std::min(x.size(), static_cast<size_t>(t1 * sr));
    int n = 0;
    size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
            if (n == 0) first = i;
            last = i;
            ++n;
        }
    return n > 1 ? (n - 1) * sr / static_cast<double>(last - first) : 0.0;
}

// Autocorrelation pitch estimate (harmonic-rich signals), parabolic interpolation of the best lag.
double autocorrHz(const std::vector<float>& x, double sr, double t0, double t1) {
    const size_t a = static_cast<size_t>(t0 * sr), b = std::min(x.size(), static_cast<size_t>(t1 * sr));
    const int minLag = static_cast<int>(sr / 2000.0), maxLag = static_cast<int>(sr / 50.0);
    const size_t n = b - a - static_cast<size_t>(maxLag) - 1;
    auto ac = [&](int lag) {
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[a + i]) * x[a + i + static_cast<size_t>(lag)];
        return s;
    };
    std::vector<double> c(static_cast<size_t>(maxLag) + 2, 0.0);
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) c[static_cast<size_t>(lag)] = ac(lag);
    // First peak above 90% of the global maximum (avoids octave errors).
    double best = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag) best = std::max(best, c[static_cast<size_t>(lag)]);
    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double y0 = c[static_cast<size_t>(lag - 1)], y1 = c[static_cast<size_t>(lag)], y2 = c[static_cast<size_t>(lag + 1)];
        if (y1 >= 0.9 * best && y1 >= y0 && y1 >= y2) {
            const double d = (y0 - y2) / (2.0 * (y0 - 2.0 * y1 + y2));
            return sr / (lag + (std::isfinite(d) ? d : 0.0));
        }
    }
    return 0.0;
}

float peakOf(const std::vector<float>& x) {
    float m = 0.0f;
    for (float v : x) m = std::max(m, std::fabs(v));
    return m;
}

bool allFinite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

Patch singleLayer(const std::string& instrument, std::map<std::string, float> iparams = {},
                  const std::string& fx = {}, std::map<std::string, float> fxParams = {}) {
    Patch p;
    Layer l;
    l.name = "L";
    l.instrument.type = instrument;
    l.instrument.params = std::move(iparams);
    if (!fx.empty()) {
        ModuleSlot f;
        f.type = fx;
        f.params = std::move(fxParams);
        l.fx.push_back(f);
    }
    p.layers.push_back(l);
    return p;
}

const char* kSineInstrument = R"(
import("stdfaust.lib");
freq = hslider("freq", 440, 20, 20000, 0.01);
gain = hslider("gain", 1, 0, 1, 0.01);
gate = button("gate");
level = hslider("level", 0.3, 0, 1, 0.01);
process = os.osc(freq) * gain * en.asr(0.001, 1, 0.01, gate) * level;
)";

const char* kGainFx = R"(
gain = hslider("gain", 0.5, 0, 1, 0.01);
process = *(gain), *(gain);
)";

// ------------------------------------------------------------------------------------------------------------
// Build a C++ plugin with scripts/build_plugin.ps1. Returns false (and SKIPs) if MSVC is missing.
bool buildDll(const fs::path& pluginsDir, const std::string& name, std::string& log) {
    const fs::path script = fs::path(KS_SOURCE_DIR) / "scripts" / "build_plugin.ps1";
    const fs::path logFile = pluginsDir / (name + "-build.log");
    const std::string cmd = "\"powershell -NoProfile -ExecutionPolicy Bypass -File \"" + pathToUtf8(script) +
                            "\" -Name " + name + " -PluginsDir \"" + pathToUtf8(pluginsDir) + "\" > \"" +
                            pathToUtf8(logFile) + "\" 2>&1\"";
    const int rc = std::system(cmd.c_str());
    std::ifstream f(logFile);
    std::stringstream ss;
    ss << f.rdbuf();
    log = ss.str();
    if (rc == 3) SKIP("MSVC not found: " + log);
    return rc == 0;
}

std::string readRepoFile(const std::string& rel) {
    std::ifstream f(fs::path(KS_SOURCE_DIR) / rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ============================================================================================================
TEST_CASE("Faust UI mapping: ids, groups, scales, readonly, voice controls", "[plugins]") {
    using K = FaustControl::Kind;
    FaustUiDesc ui;
    auto add = [&](K k, std::string label, std::vector<std::string> groups, float init, float mn, float mx, float step,
                   std::map<std::string, std::string> meta = {}) {
        FaustControl c;
        c.kind = k;
        c.label = std::move(label);
        c.groups = std::move(groups);
        c.init = init;
        c.min = mn;
        c.max = mx;
        c.step = step;
        c.meta = std::move(meta);
        ui.controls.push_back(c);
    };
    add(K::HSlider, "freq", {}, 440, 20, 20000, 0.01f);
    add(K::HSlider, "gain", {}, 0.5f, 0, 1, 0.01f);
    add(K::Button, "gate", {}, 0, 0, 1, 1);
    add(K::HSlider, "Cut-off", {"Filter"}, 1000, 20, 20000, 1, {{"scale", "log"}, {"unit", "Hz"}});
    add(K::NumEntry, "voices", {"Voice"}, 4, 1, 8, 1);
    add(K::NumEntry, "wave", {}, 1, 0, 2, 1, {{"style", "menu{'Saw':0;'Square':1;'Tri':2}"}});
    add(K::CheckBox, "on", {}, 1, 0, 1, 1);
    add(K::HBargraph, "level", {"Meters"}, 0, -60, 0, 0);
    add(K::HSlider, "amount", {"Deep", "Inner"}, 0.25f, 0, 1, 0.01f, {{"id", "amt"}, {"hidden", "1"}});

    SECTION("instrument") {
        FaustMapping m = mapFaustUi(ui, true);
        REQUIRE(m.error.empty());
        REQUIRE(m.hasGate);
        REQUIRE(m.params.size() == 6);
        REQUIRE(m.bindings.size() == ui.controls.size());
        CHECK(m.bindings[0].role == VoiceRole::Freq);
        CHECK(m.bindings[1].role == VoiceRole::Gain);
        CHECK(m.bindings[2].role == VoiceRole::Gate);
        const auto& cut = m.params[0];
        CHECK(cut.id == "filter_cut_off");
        CHECK(cut.group == "Filter");
        CHECK(cut.scale == ParamScale::Log);
        CHECK(cut.unit == "Hz");
        CHECK(cut.min == 20.0f);
        CHECK(cut.max == 20000.0f);
        CHECK(cut.def == 1000.0f);
        CHECK(m.params[1].id == "voice_voices");
        CHECK(m.params[1].scale == ParamScale::Int);
        CHECK(m.params[2].scale == ParamScale::Enum);
        CHECK(m.params[2].choices == std::vector<std::string>{"Saw", "Square", "Tri"});
        CHECK(m.params[2].def == 1.0f);
        CHECK(m.bindings[5].enumValues == std::vector<float>{0, 1, 2});
        CHECK(m.params[3].scale == ParamScale::Bool);
        CHECK(m.params[3].def == 1.0f);
        CHECK(m.params[4].id == "meters_level");
        CHECK(m.params[4].isReadOnly());
        CHECK(m.bindings[7].readOnly);
        CHECK(m.params[5].id == "amt");
        CHECK(m.params[5].group == "Inner");
        CHECK((m.params[5].flags & ParamFlags::Hidden) != 0);
        CHECK(m.groupOrder == std::vector<std::string>{"Filter", "Voice", "Meters", "Inner"});
    }
    SECTION("effect: freq/gain/gate are ordinary params") {
        FaustMapping m = mapFaustUi(ui, false);
        REQUIRE(m.error.empty());
        CHECK(m.params.size() == 9);
        CHECK(m.params[0].id == "freq");
    }
    SECTION("duplicate ids rejected") {
        add(K::HSlider, "cut off", {"filter"}, 1, 0, 2, 0.1f);
        CHECK(mapFaustUi(ui, true).error.find("duplicate param id 'filter_cut_off'") != std::string::npos);
    }
    SECTION("invalid [id:] rejected") {
        add(K::HSlider, "x", {}, 0, 0, 1, 0.1f, {{"id", "Bad-Id"}});
        CHECK(mapFaustUi(ui, true).error.find("invalid [id:Bad-Id]") != std::string::npos);
    }
    SECTION("instrument without gate rejected") {
        ui.controls.erase(ui.controls.begin() + 2);
        CHECK(mapFaustUi(ui, true).error.find("gate") != std::string::npos);
    }
    SECTION("soundfile rejected") {
        ui.hasSoundfile = true;
        CHECK(!mapFaustUi(ui, true).error.empty());
    }
    CHECK(toSnakeCase("Cut-off  Freq (Hz)") == "cut_off_freq_hz");
    CHECK(toSnakeCase("8'") == "p8");
    std::vector<std::string> labels;
    std::vector<float> values;
    CHECK(parseFaustMenu("menu{'a':0.5;'b':2}", labels, values));
    CHECK(values == std::vector<float>{0.5f, 2.0f});
    CHECK_FALSE(parseFaustMenu("menu{a:1}", labels, values));
}

TEST_CASE("Faust JIT: compile, params, cache", "[plugins][faust]") {
    requireFaust();
    TestHost t("compile");
    writeFile(t.plugins / "tiny_fx" / "tiny_fx.dsp", R"(
import("stdfaust.lib");
cutoff = hslider("h:Filter/cutoff [unit:Hz][scale:log]", 1000, 20, 20000, 1);
mode   = nentry("mode [style:menu{'LP':0;'Thru':1}]", 0, 0, 1, 1);
amount = hslider("amount [id:amt]", 0.5, 0, 1, 0.01);
steps  = nentry("steps", 4, 1, 8, 1);
on     = checkbox("on");
meter(x) = attach(x, abs(x) : hbargraph("level", 0, 1));
filt(x) = select2(mode, fi.lowpass(1, cutoff, x), x);
process = _, _ : (filt * amount * on : meter), *(steps);
)");
    t.start();
    PluginStatus s = t.st("tiny_fx");
    INFO(s.message);
    REQUIRE(s.state == "ok");
    CHECK(s.source == "faust");
    CHECK(s.moduleKind == "effect");
    CHECK(s.version == 1);
    CHECK_FALSE(s.cached);
    CHECK(s.compileMs > 0.0);
    const ModuleInfo* info = t.registry.find("plugin:tiny_fx");
    REQUIRE(info != nullptr);
    CHECK(info->kind == ModuleKind::Effect);
    REQUIRE(info->params.size() == 6);
    const ParamSpec* cut = findParam(*info, "filter_cutoff");
    REQUIRE(cut);
    CHECK(cut->scale == ParamScale::Log);
    CHECK(cut->unit == "Hz");
    CHECK(cut->group == "Filter");
    CHECK(cut->min == 20.0f);
    CHECK(cut->max == 20000.0f);
    CHECK(cut->def == 1000.0f);
    REQUIRE(findParam(*info, "mode"));
    CHECK(findParam(*info, "mode")->scale == ParamScale::Enum);
    CHECK(findParam(*info, "mode")->choices == std::vector<std::string>{"LP", "Thru"});
    REQUIRE(findParam(*info, "amt"));
    REQUIRE(findParam(*info, "steps"));
    CHECK(findParam(*info, "steps")->scale == ParamScale::Int);
    REQUIRE(findParam(*info, "on"));
    CHECK(findParam(*info, "on")->scale == ParamScale::Bool);
    REQUIRE(findParam(*info, "level"));
    CHECK(findParam(*info, "level")->isReadOnly());
    CHECK(info->uiHints["groupOrder"] == nlohmann::json::array({"Filter"}));
    CHECK(t.changed == std::vector<std::string>{"plugin:tiny_fx"});

    // Second host on the same plugins dir: machine-code cache hit.
    TestHost t2("compile2");
    fs::remove_all(t2.plugins);
    fs::copy(t.plugins, t2.plugins, fs::copy_options::recursive);
    t2.start();
    CHECK(t2.st("tiny_fx").state == "ok");
    CHECK(t2.st("tiny_fx").cached);
    CHECK(t2.registry.find("plugin:tiny_fx")->params.size() == 6);
}

TEST_CASE("Faust effect: processing, dual mono, readonly, RT", "[plugins][faust]") {
    requireFaust();
    TestHost t("fx");
    writeFile(t.plugins / "gain_fx" / "gain_fx.dsp", kGainFx);
    writeFile(t.plugins / "mono_fx" / "mono_fx.dsp",
              "gain = hslider(\"gain\", 0.5, 0, 1, 0.01);\n"
              "process = *(gain) : attach(_, gain * 2 : hbargraph(\"twice\", 0, 2));\n");
    t.start();
    REQUIRE(t.st("gain_fx").state == "ok");
    REQUIRE(t.st("mono_fx").state == "ok");

    std::vector<float> l(64), r(64);
    ProcessContext ctx;
    ChannelState ch{};
    ctx.channel = &ch;
    ctx.numSamples = 64;
    {
        auto m = t.registry.create("plugin:gain_fx");
        REQUIRE(m);
        m->prepare(48000.0, 64);
        std::fill(l.begin(), l.end(), 1.0f);
        std::fill(r.begin(), r.end(), -1.0f);
        AudioBlock b{l.data(), r.data(), 64};
        const auto before = rt::violationCount();
        {
            rt::RtScope scope;
            m->process(b, {}, ctx);
        }
        CHECK(rt::violationCount() == before);
        CHECK(l[10] == Approx(0.5f));
        CHECK(r[10] == Approx(-0.5f));
        m->params().set("gain", 0.25f);
        std::fill(l.begin(), l.end(), 1.0f);
        m->process(b, {}, ctx);
        CHECK(l[63] == Approx(0.25f));
    }
    {
        auto m = t.registry.create("plugin:mono_fx");
        REQUIRE(m);
        m->prepare(48000.0, 64);
        std::fill(l.begin(), l.end(), 1.0f);
        std::fill(r.begin(), r.end(), -2.0f);
        AudioBlock b{l.data(), r.data(), 32}; // n < maxBlock
        m->process(b, {}, ctx);
        CHECK(l[0] == Approx(0.5f));
        CHECK(r[0] == Approx(-1.0f)); // dual mono: independent channels
        CHECK(l[40] == 1.0f);         // untouched beyond n
        const int ro = m->params().indexOf("twice");
        REQUIRE(ro >= 0);
        CHECK(m->params().get(ro) == Approx(1.0f));
    }
}

TEST_CASE("Faust instrument: polyphony, pitch, release, bend", "[plugins][faust]") {
    requireFaust();
    TestHost t("inst");
    writeFile(t.plugins / "sine_inst" / "sine_inst.dsp", kSineInstrument);
    writeFile(t.plugins / "sine_inst" / "plugin.json", R"({"polyphony": 4, "category": "Test"})");
    t.start();
    PluginStatus s = t.st("sine_inst");
    INFO(s.message);
    REQUIRE(s.state == "ok");
    CHECK(s.moduleKind == "instrument");
    const ModuleInfo* info = t.registry.find("plugin:sine_inst");
    REQUIRE(info);
    REQUIRE(info->params.size() == 1); // freq/gain/gate are voice controls
    CHECK(info->params[0].id == "level");
    CHECK(info->category == "Test");

    SECTION("rendered pitch (offline renderer, real engine)") {
        RenderOptions opt;
        opt.tailSeconds = 0.3;
        const auto before = rt::violationCount();
        RenderResult r = OfflineRenderer::render(singleLayer("plugin:sine_inst"),
                                                 OfflineRenderer::notesToEvents({{69, 0.0, 0.5, 100}}), opt, t.registry);
        CHECK(rt::violationCount() == before);
        CHECK(allFinite(r.left));
        CHECK(zeroCrossingHz(r.left, r.sampleRate, 0.1, 0.45) == Approx(440.0).margin(0.5));
        CHECK(peakOf(r.left) > 0.1f);
        // Released: silent at the end.
        float tail = 0.0f;
        for (size_t i = r.left.size() - 2400; i < r.left.size(); ++i) tail = std::max(tail, std::fabs(r.left[i]));
        CHECK(tail < 1e-4f);
    }
    SECTION("voices, stealing, release end, pitch bend") {
        auto m = t.registry.create("plugin:sine_inst");
        REQUIRE(m);
        m->prepare(48000.0, 256);
        std::vector<float> l(256), r(256);
        ChannelState ch{};
        ProcessContext ctx;
        ctx.channel = &ch;
        auto run = [&](std::vector<MidiEvent> ev) {
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            AudioBlock b{l.data(), r.data(), 256};
            rt::RtScope scope;
            m->process(b, MidiEventSpan(ev.data(), ev.size()), ctx);
        };
        run({MidiEvent::noteOn(60, 100), MidiEvent::noteOn(64, 100), MidiEvent::noteOn(67, 100)});
        CHECK(m->activeVoices() == 3);
        run({MidiEvent::noteOn(70, 100), MidiEvent::noteOn(72, 100)}); // 5 notes, polyphony 4
        CHECK(m->activeVoices() == 4);
        CHECK(l == r); // mono plugin duplicated to both channels
        run({MidiEvent::allNotesOff()});
        for (int i = 0; i < 40; ++i) run({});
        CHECK(m->activeVoices() == 0);

        // Pitch bend +1 = +2 semitones (440 -> 493.88 Hz).
        const auto before = rt::violationCount();
        std::vector<float> acc;
        ch.pitchBend = 1.0f;
        run({MidiEvent::noteOn(69, 127)});
        for (int i = 0; i < 60; ++i) {
            run({});
            acc.insert(acc.end(), l.begin(), l.end());
        }
        CHECK(rt::violationCount() == before);
        CHECK(zeroCrossingHz(acc, 48000.0, 0.0, 0.3) == Approx(493.88).margin(0.5));
    }
}

TEST_CASE("Faust hot reload: new version picked up, params preserved, live graph rebuilt", "[plugins][faust]") {
    requireFaust();
    TestHost t("reload");
    const fs::path dsp = t.plugins / "hr_fx" / "hr_fx.dsp";
    writeFile(dsp, kGainFx);
    t.start();
    REQUIRE(t.st("hr_fx").state == "ok");

    Engine engine;
    engine.prepare(48000.0, 64);
    Session session(engine, t.registry, AppPaths::fromRoot(t.root));
    t.host->onModulesChanged = [&](const std::vector<std::string>& ids) { session.refreshModules(ids); };
    session.setPatch(singleLayer("basic", {}, "plugin:hr_fx", {{"gain", 0.25f}}));
    const NodeId fxNode = session.model().patch().layers[0].fx[0].node;
    Module* before = engine.latestGraph()->findModule(fxNode);
    REQUIRE(before);
    CHECK(before->params().get(before->params().indexOf("gain")) == Approx(0.25f));

    // v2: one more param; the existing one keeps its id.
    writeFile(dsp, R"(
gain = hslider("gain", 0.5, 0, 1, 0.01);
trim = hslider("trim", 0.75, 0, 1, 0.01);
process = *(gain * trim), *(gain * trim);
)");
    bumpMtime(dsp, 5);
    t.host->poll(); // sync host: recompiles inside poll() once the scan interval elapsed
    t.host->rescan();
    PluginStatus s = t.st("hr_fx");
    INFO(s.message);
    REQUIRE(s.state == "ok");
    CHECK(s.version == 2);
    REQUIRE(t.registry.find("plugin:hr_fx")->params.size() == 2);

    const ModuleSlot& slot = session.model().patch().layers[0].fx[0];
    CHECK(slot.node == fxNode);
    CHECK(slot.params.at("gain") == Approx(0.25f)); // preserved
    CHECK(slot.params.at("trim") == Approx(0.75f)); // new param: default
    Module* after = engine.latestGraph()->findModule(fxNode);
    REQUIRE(after);
    CHECK(after != before);
    CHECK(&after->info() == t.registry.find("plugin:hr_fx"));
    CHECK(after->params().get(after->params().indexOf("gain")) == Approx(0.25f));
    CHECK(after->params().get(after->params().indexOf("trim")) == Approx(0.75f));

    // Unchanged file: no new version.
    t.host->rescan();
    CHECK(t.st("hr_fx").version == 2);

    // Removing the plugin unregisters it.
    fs::remove_all(t.plugins / "hr_fx");
    t.host->rescan();
    CHECK(t.registry.find("plugin:hr_fx") == nullptr);
    CHECK_FALSE(t.host->status("hr_fx").has_value());
    engine.collectGarbage();
}

TEST_CASE("Faust compile errors are reported, previous version keeps working", "[plugins][faust]") {
    requireFaust();
    TestHost t("err");
    const fs::path dsp = t.plugins / "bad_fx" / "bad_fx.dsp";
    writeFile(dsp, "process = _ , ;\n");
    writeFile(t.plugins / "bom_fx" / "bom_fx.dsp", "\xEF\xBB\xBFprocess = _, _;\n");
    writeFile(t.plugins / "chan_fx" / "chan_fx.dsp", "process = _, _, _;\n"); // 3 in, 3 out
    t.start();
    PluginStatus s = t.st("bad_fx");
    CHECK(s.state == "error");
    CHECK(s.message.find("ERROR") != std::string::npos);
    CHECK(s.message.find("bad_fx.dsp") != std::string::npos);
    CHECK(t.registry.find("plugin:bad_fx") == nullptr);
    CHECK(t.st("bom_fx").message.find("BOM") != std::string::npos);
    CHECK(t.st("chan_fx").message.find("1-2 inputs") != std::string::npos);

    writeFile(dsp, kGainFx); // fixed
    bumpMtime(dsp, 5);
    t.host->rescan();
    CHECK(t.st("bad_fx").state == "ok");
    CHECK(t.st("bad_fx").version == 1);
    REQUIRE(t.registry.find("plugin:bad_fx"));

    writeFile(dsp, "process = undefined_symbol;\n"); // broken again
    bumpMtime(dsp, 10);
    t.host->rescan();
    s = t.st("bad_fx");
    CHECK(s.state == "error");
    CHECK(s.message.find("undefined_symbol") != std::string::npos);
    CHECK(t.registry.find("plugin:bad_fx") != nullptr); // v1 still registered
    CHECK(s.version == 1);

    std::string err;
    CHECK_FALSE(t.host->reload("../etc", err));
    CHECK_FALSE(t.host->reload("nope", err));
    CHECK(err.find("unknown") != std::string::npos);
    CHECK(t.host->reload("bad_fx", err));
}

TEST_CASE("Faust async loader thread", "[plugins][faust]") {
    requireFaust();
    TestHost t("async");
    writeFile(t.plugins / "gain_fx" / "gain_fx.dsp", kGainFx);
    t.start(true);
    CHECK(t.st("gain_fx").state == "compiling");
    REQUIRE(t.host->waitIdle(60000));
    CHECK(t.st("gain_fx").state == "ok");
    CHECK(t.registry.find("plugin:gain_fx") != nullptr);
}

TEST_CASE("Example plugins: compile, render, no allocation in compute", "[plugins][faust]") {
    requireFaust();
    TestHost t("examples");
    for (const char* name : {"fx_simple_tremolo", "faust_pluck", "moog_vcf", "string_machine"}) {
        const fs::path src = fs::path(KS_SOURCE_DIR) / "plugins" / name;
        fs::copy(src, t.plugins / name, fs::copy_options::recursive);
    }
    t.start();
    for (const char* name : {"fx_simple_tremolo", "faust_pluck", "moog_vcf", "string_machine"}) {
        INFO(name);
        PluginStatus s = t.st(name);
        INFO(s.message);
        REQUIRE(s.state == "ok");
        // Faust's generated compute must not call an allocator (RT rule; the JIT code is invisible to the
        // operator-new hook, so check the IR).
        auto v = std::static_pointer_cast<const FaustPluginVersion>(t.host->current(name));
        REQUIRE(v);
        const std::string ir = faust::irText(*v->factory);
        REQUIRE(!ir.empty());
        for (const char* fn : {"@malloc", "@calloc", "@realloc", "@free(", "@_Znw", "@_Zna", "@_Zdl", "@_Zda"})
            CHECK(ir.find(fn) == std::string::npos);

        const bool instrument = s.moduleKind == "instrument";
        Patch p = instrument ? singleLayer(std::string("plugin:") + name)
                             : singleLayer("basic", {}, std::string("plugin:") + name);
        RenderOptions opt;
        opt.tailSeconds = 2.0;
        const auto before = rt::violationCount();
        RenderResult r = OfflineRenderer::render(p, OfflineRenderer::standardTestEvents(), opt, t.registry);
        CHECK(r.warnings.empty());
        CHECK(rt::violationCount() == before);
        CHECK(allFinite(r.left));
        CHECK(allFinite(r.right));
        CHECK(peakOf(r.left) > 0.01f);
        CHECK(peakOf(r.left) <= 1.0f);
    }
    // faust_pluck is tuned: A4 -> 440 Hz.
    RenderResult r = OfflineRenderer::render(singleLayer("plugin:faust_pluck"),
                                             OfflineRenderer::notesToEvents({{69, 0.0, 1.0, 100}}), {}, t.registry);
    CHECK(autocorrHz(r.left, r.sampleRate, 0.1, 0.6) == Approx(440.0).margin(1.0));
}

// ============================================================================================================
TEST_CASE("DLL plugin: build, load, process, state, reload, unload", "[plugins][dll]") {
    TestHost t("dll");
    const fs::path dir = t.plugins / "cpp_bitcrusher";
    fs::copy(fs::path(KS_SOURCE_DIR) / "plugins" / "cpp_bitcrusher", dir, fs::copy_options::recursive);
    std::string log;
    REQUIRE(buildDll(t.plugins, "cpp_bitcrusher", log));
    INFO(log);
    t.start();
    PluginStatus s = t.st("cpp_bitcrusher");
    INFO(s.message);
    REQUIRE(s.state == "ok");
    CHECK(s.source == "dll");
    CHECK(s.moduleKind == "effect");
    const ModuleInfo* info = t.registry.find("plugin:cpp_bitcrusher");
    REQUIRE(info);
    REQUIRE(findParam(*info, "bits"));
    CHECK(findParam(*info, "downsample")->scale == ParamScale::Int);
    CHECK(findParam(*info, "dither")->choices == std::vector<std::string>{"Off", "TPDF"});
    CHECK(findParam(*info, "clip")->isReadOnly());
    CHECK(info->uiHints["groupOrder"] == nlohmann::json::array({"Crush", "Output"}));

    std::weak_ptr<PluginLibrary> lib1;
    {
        auto v1 = std::static_pointer_cast<const DllPluginVersion>(t.host->current("cpp_bitcrusher"));
        lib1 = v1->lib;
    }
    std::unique_ptr<Module> m = t.registry.create("plugin:cpp_bitcrusher");
    REQUIRE(m);
    m->params().set("bits", 2.0f); // levels = 2 -> values in {-1, -0.5, 0, 0.5, 1}
    m->params().set("mix", 1.0f);
    m->prepare(48000.0, 64);
    std::vector<float> l(64), r(64);
    for (int i = 0; i < 64; ++i) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.3f;
    AudioBlock b{l.data(), r.data(), 64};
    ProcessContext ctx;
    const auto before = rt::violationCount();
    {
        rt::RtScope scope;
        m->process(b, {}, ctx);
    }
    CHECK(rt::violationCount() == before);
    CHECK(l[63] == Approx(0.5f).margin(0.02f));
    m->params().set("output_db", 12.0f); // -> clipped readout after the next block
    for (int i = 0; i < 64; ++i) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 1.5f;
    m->process(b, {}, ctx);
    CHECK(m->params().get(m->params().indexOf("clip")) == Approx(1.0f));

    // State blob round trip.
    const std::vector<unsigned char> seed = {0x78, 0x56, 0x34, 0x12};
    nlohmann::json st = {{"blob", DllModule::base64Encode({0x01, 0x02, 0x03, 0x04})}};
    m->loadState(st);
    CHECK(m->saveState() == st);
    std::vector<unsigned char> dec;
    CHECK(DllModule::base64Decode(DllModule::base64Encode({1, 2, 3, 4, 5}), dec));
    CHECK(dec == std::vector<unsigned char>{1, 2, 3, 4, 5});
    (void)seed;

    // Reload: new source -> new hash DLL -> version 2; v1 library stays loaded while v1 modules live.
    {
        std::ifstream f(dir / "cpp_bitcrusher.cpp");
        std::stringstream ss;
        ss << f.rdbuf();
        std::string src = ss.str();
        const std::string from = "\"Bitcrusher\",";
        REQUIRE(src.find(from) != std::string::npos);
        src.replace(src.find(from), from.size(), "\"Bitcrusher v2\",");
        writeFile(dir / "cpp_bitcrusher.cpp", src);
    }
    REQUIRE(buildDll(t.plugins, "cpp_bitcrusher", log));
    t.host->rescan();
    s = t.st("cpp_bitcrusher");
    INFO(s.message);
    REQUIRE(s.state == "ok");
    CHECK(s.version == 2);
    CHECK(t.registry.find("plugin:cpp_bitcrusher")->displayName == "Bitcrusher v2");
    CHECK_FALSE(lib1.expired()); // still used by `m`
    m->process(b, {}, ctx);      // old module keeps working
    m.reset();
    CHECK(lib1.expired()); // FreeLibrary after the last v1 instance
    {
        std::error_code ec;
        int copies = 0;
        for (const auto& e : fs::directory_iterator(t.root / "tmp", ec)) copies += e.path().extension() == ".dll";
        CHECK(copies == 1); // only v2's temp copy remains
    }

    // Unload: removing the plugin directory unregisters it and frees the library.
    std::weak_ptr<PluginLibrary> lib2 =
        std::static_pointer_cast<const DllPluginVersion>(t.host->current("cpp_bitcrusher"))->lib;
    fs::remove_all(dir);
    t.host->rescan();
    CHECK(t.registry.find("plugin:cpp_bitcrusher") == nullptr);
    CHECK(lib2.expired());
}

TEST_CASE("DLL plugin: SEH fault isolation (crash -> muted, engine survives)", "[plugins][dll]") {
    TestHost t("seh");
    writeFile(t.plugins / "crashy" / "crashy.cpp", R"(
#include <keysynth/plugin_abi.h>
#include <new>
namespace {
const ks_param_spec kParams[] = {
    {sizeof(ks_param_spec), "crash", "Crash", "", "", 0.0f, 1.0f, 0.0f, KS_SCALE_BOOL, 0.0f, 0, nullptr, 0},
};
struct S { float crash = 0.0f; };
ks_instance create() { return new (std::nothrow) S(); }
void destroy(ks_instance i) { delete static_cast<S*>(i); }
void prepare(ks_instance, double, int32_t) {}
void reset(ks_instance) {}
void setParam(ks_instance i, uint32_t, float v) { static_cast<S*>(i)->crash = v; }
void process(ks_instance i, float** io, int32_t n, const ks_event*, int32_t) {
    if (static_cast<S*>(i)->crash > 0.5f) {
        volatile int* p = nullptr;
        *p = 42; // access violation
    }
    for (int32_t s = 0; s < n; ++s) { io[0][s] *= 0.5f; io[1][s] *= 0.5f; }
}
const ks_plugin_descriptor kDesc = {sizeof(ks_plugin_descriptor), KS_PLUGIN_ABI_VERSION, "crashy", "Crashy", "Test",
    KS_KIND_EFFECT, kParams, 1, &create, &destroy, &prepare, &reset, &process, &setParam, nullptr, nullptr, nullptr,
    nullptr, nullptr};
}
extern "C" KS_PLUGIN_EXPORT const ks_plugin_descriptor* ks_get_plugin(void) { return &kDesc; }
)");
    std::string log;
    REQUIRE(buildDll(t.plugins, "crashy", log));
    INFO(log);
    t.start();
    REQUIRE(t.st("crashy").state == "ok");

    // Layer 1: basic synth (keeps sounding). Layer 2: basic -> crashy fx that crashes.
    Patch p = singleLayer("basic");
    Patch p2 = singleLayer("basic", {}, "plugin:crashy", {{"crash", 1.0f}});
    p.layers.push_back(p2.layers[0]);
    RenderOptions opt;
    opt.tailSeconds = 0.5;
    RenderResult r = OfflineRenderer::render(p, OfflineRenderer::notesToEvents({{60, 0.0, 0.5, 100}}), opt, t.registry);
    CHECK(allFinite(r.left));
    CHECK(peakOf(r.left) > 0.01f); // engine survived, layer 1 audible

    t.host->poll();
    PluginStatus s = t.st("crashy");
    CHECK(s.state == "faulted");
    CHECK(s.message.find("0xC0000005") != std::string::npos);

    // Module level: faulted instance outputs silence from then on.
    auto m = t.registry.create("plugin:crashy");
    REQUIRE(m);
    m->prepare(48000.0, 32);
    std::vector<float> l(32, 1.0f), rr(32, 1.0f);
    AudioBlock b{l.data(), rr.data(), 32};
    m->process(b, {}, {});
    CHECK(l[0] == Approx(0.5f));
    m->params().set("crash", 1.0f);
    m->process(b, {}, {});
    std::fill(l.begin(), l.end(), 1.0f);
    m->params().set("crash", 0.0f);
    m->process(b, {}, {});
    CHECK(l[0] == 0.0f); // muted for good
    CHECK(static_cast<DllModule*>(m.get())->faulted());
}
