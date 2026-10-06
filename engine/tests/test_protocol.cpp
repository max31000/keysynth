// Protocol handler tests without network: feed JSON to ProtocolHandler, inspect outgoing messages.

#include "control/ControlServer.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace ks;
using nlohmann::json;

namespace {

struct FakeAudio final : AudioControl {
    AudioStatus st{"FakeType", "Fake Device", 48000.0, 64, 1.0, 2.5, true};
    std::string lastType, lastName;
    AudioStatus status() const override { return st; }
    AudioDeviceList listDevices() override {
        AudioDeviceList l;
        l.types = {"FakeType"};
        AudioDeviceList::TypeDevices td;
        td.type = "FakeType";
        td.names = {"Fake Device", "Other"};
        td.sampleRates = {48000.0};
        td.bufferSizes = {64, 128};
        l.available = {td};
        return l;
    }
    std::string setDevice(const std::string& type, const std::string& name, double, int) override {
        if (type != "FakeType") return "unknown type";
        lastType = type;
        lastName = name;
        st.name = name;
        return {}; // like a driver that only offers its control-panel buffer: bufferSize stays 64
    }
    int panelOpens = 0;
    std::string openControlPanel() override {
        if (!st.hasControlPanel) return "no panel";
        ++panelOpens;
        return {};
    }
};

struct FakeMidi final : MidiControl {
    int rescans = 0;
    std::vector<std::string> inputs() const override { return {"Digital Piano"}; }
    void rescan() override { ++rescans; }
};

struct Fixture {
    std::filesystem::path tmpRoot;
    Engine engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<ProtocolHandler> handler;
    FakeAudio audio;
    FakeMidi midi;

    Fixture() {
        // Temp root with a copy of the factory presets so save_preset never touches the repo's userdata.
        tmpRoot = std::filesystem::temp_directory_path() / ("ks-proto-test-" + std::to_string(std::rand()));
        std::filesystem::remove_all(tmpRoot);
        std::filesystem::create_directories(tmpRoot / "presets");
        std::filesystem::copy(std::filesystem::path(KS_SOURCE_DIR) / "presets" / "factory", tmpRoot / "presets" / "factory",
                              std::filesystem::copy_options::recursive);
        std::filesystem::copy(std::filesystem::path(KS_SOURCE_DIR) / "presets" / "patterns", tmpRoot / "presets" / "patterns",
                              std::filesystem::copy_options::recursive);
        engine.prepare(48000.0, 64);
        session = std::make_unique<Session>(engine, defaultRegistry(), AppPaths::fromRoot(tmpRoot));
        session->setAudio(&audio);
        session->setMidi(&midi);
        session->rebuild(false);
        handler = std::make_unique<ProtocolHandler>(*session);
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(tmpRoot, ec);
    }
    std::vector<Outgoing> send(const json& j) { return handler->handle(j); }
    json reply(const json& j) {
        auto out = send(j);
        for (auto& o : out)
            if (o.target == Outgoing::Target::Reply) return o.message;
        return nullptr;
    }
    NodeId firstLayer() { return session->model().patch().layers.at(0).node; }
};

} // namespace

TEST_CASE("protocol: hello returns state with id", "[protocol]") {
    Fixture f;
    const json r = f.reply({{"type", "hello"}, {"id", 1}, {"client", "ui"}, {"version", "0"}});
    REQUIRE(r["type"] == "state");
    REQUIRE(r["id"] == 1);
    REQUIRE(r["patch"]["layers"].size() == 1);
    REQUIRE(r["audio"]["name"] == "Fake Device");
    REQUIRE(r["audio"]["outputLatencyMs"] == 2.5);
    for (const char* k : {"playing", "tempo", "metronome", "metronome_volume", "pattern", "drums", "drums_volume",
                          "swing", "time_sig", "count_in", "pattern_edited"})
        REQUIRE(r["transport"].contains(k));
    REQUIRE(r["transport"]["pattern"] == "presets/patterns/basic-rock.json"); // default groove
    REQUIRE(r["patch"]["rhythm"]["node"].get<int>() > 0);
}

TEST_CASE("protocol: catalog", "[protocol]") {
    Fixture f;
    const json r = f.reply({{"type", "get_catalog"}, {"id", 2}});
    REQUIRE(r["type"] == "catalog_ok");
    bool basic = false, limiter = false;
    for (const auto& m : r["modules"]) {
        if (m["typeId"] == "basic") {
            basic = true;
            REQUIRE(m["kind"] == "instrument");
            REQUIRE(m["params"].size() >= 6);
            REQUIRE(m["params"][0]["scale"] == "enum");
        }
        if (m["typeId"] == "limiter") limiter = m["kind"] == "effect";
    }
    REQUIRE(basic);
    REQUIRE(limiter);
}

TEST_CASE("protocol: set_param updates model + live graph and echoes to others", "[protocol]") {
    Fixture f;
    const NodeId instr = f.session->model().patch().layers[0].instrument.node;
    auto out = f.send({{"type", "set_param"}, {"node", instr}, {"param", "cutoff"}, {"value", 1234.5}});
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].target == Outgoing::Target::Others);
    REQUIRE(out[0].message["type"] == "param");
    REQUIRE(f.session->model().patch().layers[0].instrument.params.at("cutoff") == 1234.5f);
    Module* m = f.engine.latestGraph()->findModule(instr);
    REQUIRE(m->params().get(m->params().indexOf("cutoff")) == 1234.5f);

    // master volume via node 0
    f.send({{"type", "set_param"}, {"node", 0}, {"param", "volume_db"}, {"value", -7}});
    REQUIRE(f.session->model().patch().master.volumeDb == -7.0f);
    REQUIRE(f.engine.latestGraph()->masterVolumeDb.load() == -7.0f);

    // errors
    REQUIRE(f.reply({{"type", "set_param"}, {"id", 3}, {"node", instr}, {"param", "nope"}, {"value", 1}})["code"] == "unknown_param");
    REQUIRE(f.reply({{"type", "set_param"}, {"node", 999}, {"param", "x"}, {"value", 1}})["code"] == "not_found");
    REQUIRE(f.reply({{"type", "set_param"}, {"node", "abc"}, {"param", "x"}, {"value", 1}})["code"] == "bad_request");
}

TEST_CASE("protocol: structural ops", "[protocol]") {
    Fixture f;
    json r = f.reply({{"type", "add_layer"}, {"id", 4}});
    REQUIRE(r["type"] == "state");
    REQUIRE(r["id"] == 4);
    REQUIRE(r["patch"]["layers"].size() == 2);
    const NodeId l2 = r["patch"]["layers"][1]["node"].get<NodeId>();

    r = f.reply({{"type", "add_fx"}, {"layer", l2}, {"module", "gain"}});
    REQUIRE(r["patch"]["layers"][1]["fx"].size() == 1);
    const NodeId fx = r["patch"]["layers"][1]["fx"][0]["node"].get<NodeId>();
    r = f.reply({{"type", "add_fx"}, {"layer", 0}, {"module", "limiter"}, {"index", 0}});
    REQUIRE(r["patch"]["master"]["fx"].size() == 1);
    REQUIRE(f.reply({{"type", "add_fx"}, {"layer", l2}, {"module", "basic"}})["type"] == "error");
    REQUIRE(f.reply({{"type", "add_fx"}, {"layer", l2}, {"module", "zzz"}})["code"] == "unknown_module");

    REQUIRE(f.send({{"type", "set_fx_bypass"}, {"node", fx}, {"bypass", true}}).empty());
    REQUIRE(f.engine.latestGraph()->findModuleNode(fx)->bypass.load());

    REQUIRE(f.send({{"type", "set_zone"}, {"layer", l2}, {"zone", {{"key_lo", 60}, {"transpose", 12}}}}).empty());
    REQUIRE(f.session->model().findLayer(l2)->zone.keyLo == 60);
    REQUIRE(f.session->model().findLayer(l2)->zone.keyHi == 127); // partial update keeps the rest
    REQUIRE(f.engine.latestGraph()->findLayer(l2)->transpose.load() == 12);

    r = f.reply({{"type", "move_fx"}, {"node", fx}, {"to", 0}});
    REQUIRE(r["type"] == "state");
    r = f.reply({{"type", "set_instrument"}, {"layer", l2}, {"module", "basic"}});
    REQUIRE(r["type"] == "state");
    r = f.reply({{"type", "remove_fx"}, {"node", fx}});
    REQUIRE(r["patch"]["layers"][1]["fx"].empty());
    r = f.reply({{"type", "remove_layer"}, {"layer", l2}});
    REQUIRE(r["patch"]["layers"].size() == 1);
    REQUIRE(f.reply({{"type", "remove_layer"}, {"layer", l2}})["code"] == "not_found");
}

TEST_CASE("protocol: presets list/load/save, get/set patch", "[protocol]") {
    Fixture f;
    json r = f.reply({{"type", "list_presets"}, {"id", 5}});
    REQUIRE(r["type"] == "list_presets_ok");
    REQUIRE(r["presets"].size() >= 2);
    std::string split;
    for (const auto& p : r["presets"]) // the `basic` split: the test below edits its `cutoff` param
        if (p["category"] == "Splits & Layers" && p["path"].get<std::string>().find("basic-") != std::string::npos)
            split = p["path"];
    REQUIRE(!split.empty());

    auto out = f.send({{"type", "load_preset"}, {"id", 6}, {"path", split}});
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].message["type"] == "load_preset_ok");
    REQUIRE(out[1].message["type"] == "state");
    REQUIRE(out[1].target == Outgoing::Target::Broadcast);
    REQUIRE(out[1].message["patch"]["layers"].size() == 2);
    REQUIRE(out[1].message["presetPath"] == split);

    REQUIRE(f.reply({{"type", "load_preset"}, {"path", "../../etc/passwd"}})["code"] == "bad_path");
    REQUIRE(f.reply({{"type", "load_preset"}, {"path", "presets/factory/missing.json"}})["code"] == "not_found");

    r = f.reply({{"type", "save_preset"}, {"id", 7}, {"name", "My Split"}, {"category", "Splits & Layers"}});
    REQUIRE(r["type"] == "save_preset_ok");
    REQUIRE(r["path"] == "userdata/presets/my-split.json");
    REQUIRE(f.reply({{"type", "save_preset"}, {"name", "My Split"}})["code"] == "exists");
    REQUIRE(f.reply({{"type", "save_preset"}, {"name", "My Split"}, {"overwrite", true}})["type"] == "save_preset_ok");
    bool found = false;
    const json listed = f.reply({{"type", "list_presets"}})["presets"];
    for (const auto& p : listed) found = found || (p["name"] == "My Split" && p["factory"] == false);
    INFO(listed.dump());
    REQUIRE(found);

    r = f.reply({{"type", "get_patch"}, {"id", 8}});
    REQUIRE(r["type"] == "state");
    json patch = r["patch"];
    patch["layers"][0]["instrument"]["params"]["cutoff"] = 500;
    r = f.reply({{"type", "set_patch"}, {"id", 9}, {"patch", patch}});
    REQUIRE(r["type"] == "state");
    REQUIRE(r["patch"]["layers"][0]["instrument"]["params"]["cutoff"] == 500.0);
    REQUIRE(r["dirty"] == true);
}

TEST_CASE("protocol: fm .syx voice is adopted into the patch, later edits survive rebuilds", "[protocol][fm]") {
    Fixture f;
    json patch = f.reply({{"type", "get_patch"}})["patch"];
    patch["layers"][0]["instrument"] = {{"type", "fm"},
                                        {"params", json::object()},
                                        {"state", {{"syx", "assets/dx7/keysynth-fm-factory.syx"}, {"voice", 0}}}};
    json r = f.reply({{"type", "set_patch"}, {"patch", patch}});
    REQUIRE(r["type"] == "state");
    const json& inst = r["patch"]["layers"][0]["instrument"];
    REQUIRE(inst["state"]["applied"] == true);
    REQUIRE(inst["params"]["alg"] == 5.0); // BALLAD EP, not the default algorithm 1
    const NodeId node = inst["node"].get<NodeId>();
    auto moduleParam = [&](const char* id) {
        const Module* m = f.session->engine().latestGraph()->findModule(node);
        REQUIRE(m != nullptr);
        return m->params().get(m->params().indexOf(id));
    };
    const Module* before = f.session->engine().latestGraph()->findModule(node);

    // Edit, then a structural rebuild with reuse (add fx) and one without (device restart): the edit stays.
    f.send({{"type", "set_param"}, {"node", node}, {"param", "alg"}, {"value", 17}});
    r = f.reply({{"type", "add_fx"}, {"layer", f.firstLayer()}, {"module", "gain"}});
    REQUIRE(r["type"] == "state");
    REQUIRE(f.session->engine().latestGraph()->findModule(node) == before); // reused (state unchanged)
    REQUIRE(moduleParam("alg") == 17.0f);
    f.session->rebuild(false); // new module instance: loadState must skip the syx now
    REQUIRE(moduleParam("alg") == 17.0f);
    REQUIRE(f.reply({{"type", "get_patch"}})["patch"]["layers"][0]["instrument"]["params"]["alg"] == 17.0);
}

TEST_CASE("protocol: note, cc, transport, panic, devices", "[protocol]") {
    Fixture f;
    REQUIRE(f.send({{"type", "note"}, {"on", true}, {"note", 60}, {"velocity", 100}}).empty());
    REQUIRE(f.send({{"type", "cc"}, {"cc", 64}, {"value", 127}, {"channel", 2}}).empty());
    REQUIRE(f.reply({{"type", "note"}, {"on", true}, {"note", 200}})["code"] == "bad_request");

    // Notes go through the inbox into the audio path and show up as midi events.
    std::vector<float> l(64), r(64);
    AudioBlock b{l.data(), r.data(), 64};
    f.engine.processBlock(b);
    const json midi = f.session->pollMidi();
    REQUIRE(midi["type"] == "midi");
    REQUIRE(midi["notes"][0]["note"] == 60);
    REQUIRE(midi["notes"][0]["on"] == true);
    const json tele = f.session->pollTelemetry();
    REQUIRE(tele["type"] == "telemetry");
    REQUIRE(tele["meters"]["master"].size() == 2);
    REQUIRE(tele["cpu"].get<double>() <= 1.0);

    auto out = f.send({{"type", "transport"}, {"tempo", 140}, {"playing", true}, {"metronome", true}, {"metronome_volume", 0.3}});
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].target == Outgoing::Target::Others);
    REQUIRE(f.engine.transport().tempo() == 140.0);
    REQUIRE(f.engine.transport().playing());
    REQUIRE(f.engine.metronome().enabled());
    REQUIRE(f.reply({{"type", "transport"}, {"pattern", "../../x.json"}})["code"] == "bad_path");
    REQUIRE(f.reply({{"type", "transport"}, {"pattern", "presets/patterns/nope.json"}})["code"] == "not_found");
    REQUIRE(f.send({{"type", "panic"}}).empty());

    json d = f.reply({{"type", "list_devices"}, {"id", 10}});
    REQUIRE(d["type"] == "devices");
    REQUIRE(d["id"] == 10);
    REQUIRE(d["midiInputs"][0] == "Digital Piano");
    REQUIRE(d["available"][0]["bufferSizes"].size() == 2);
    d = f.reply({{"type", "rescan_midi"}});
    REQUIRE(f.midi.rescans == 1);
    auto setOut = f.send({{"type", "set_audio_device"}, {"id", 11}, {"device_type", "FakeType"}, {"name", "Other"}, {"buffer_size", 128}});
    d = f.reply({{"type", "list_devices"}});
    REQUIRE(f.audio.lastName == "Other");
    // Reply first (with id), then a warning event without id: the fake "driver" kept its 64-sample buffer.
    std::vector<json> replies;
    for (auto& o : setOut)
        if (o.target == Outgoing::Target::Reply) replies.push_back(o.message);
    REQUIRE(replies.size() == 2);
    REQUIRE(replies[0]["type"] == "devices");
    REQUIRE(replies[0]["id"] == 11);
    REQUIRE(replies[1]["type"] == "log");
    REQUIRE(replies[1]["level"] == "warn");
    REQUIRE(!replies[1].contains("id"));
    REQUIRE(replies[1]["message"].get<std::string>().find("offered: 64, 128") != std::string::npos);
    REQUIRE(f.reply({{"type", "set_audio_device"}, {"device_type", "Nope"}, {"name", "x"}})["code"] == "device_error");

    // open_audio_panel: not_available without a panel; ok (and forwarded to the host) with one.
    REQUIRE(f.reply({{"type", "list_devices"}})["current"]["hasControlPanel"] == false);
    REQUIRE(f.reply({{"type", "open_audio_panel"}, {"id", 12}})["code"] == "not_available");
    f.audio.st.hasControlPanel = true;
    REQUIRE(f.reply({{"type", "hello"}})["audio"]["hasControlPanel"] == true);
    const json ok = f.reply({{"type", "open_audio_panel"}, {"id", 13}});
    REQUIRE(ok["type"] == "open_audio_panel_ok");
    REQUIRE(ok["id"] == 13);
    REQUIRE(f.audio.panelOpens == 1);
}

TEST_CASE("protocol: rhythm (transport fields, patterns, kit node)", "[protocol][sequencer]") {
    Fixture f;
    // list_patterns
    json r = f.reply({{"type", "list_patterns"}, {"id", 1}});
    REQUIRE(r["type"] == "list_patterns_ok");
    REQUIRE(r["patterns"].size() >= 10);
    bool money = false;
    for (const auto& p : r["patterns"])
        if (p["path"] == "presets/patterns/money-7-4.json") money = p["time_sig"] == json::array({7, 4}) && p["factory"] == true;
    REQUIRE(money);

    // load a pattern via transport: tempo, meter, kit applied; state to all
    auto out = f.send({{"type", "transport"}, {"pattern", "presets/patterns/du-hast-stomp.json"}});
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].target == Outgoing::Target::Broadcast);
    REQUIRE(out[0].message["transport"]["pattern"] == "presets/patterns/du-hast-stomp.json");
    REQUIRE(out[0].message["transport"]["tempo"] == 125.0);
    const NodeId kit = f.session->model().patch().rhythm.drums.node;
    REQUIRE(f.session->model().patch().rhythm.drums.params.at("kit") == 3.0f);
    REQUIRE(f.engine.latestGraph()->findModule(kit)->params().get(0) == 3.0f);
    REQUIRE(f.session->model().patch().rhythm.pattern == "presets/patterns/du-hast-stomp.json");

    // other fields
    out = f.send({{"type", "transport"}, {"drums", false}, {"drums_volume", 0.4}, {"swing", 0.25}, {"count_in", true}});
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].target == Outgoing::Target::Others);
    const json& t = out[0].message["transport"];
    REQUIRE(t["drums"] == false);
    REQUIRE(std::fabs(t["drums_volume"].get<double>() - 0.4) < 1e-6);
    REQUIRE(std::fabs(t["swing"].get<double>() - 0.25) < 1e-6);
    REQUIRE(t["count_in"] == true);

    // time signature resizes the current pattern
    out = f.send({{"type", "transport"}, {"time_sig", {7, 8}}});
    REQUIRE(out[0].target == Outgoing::Target::Broadcast);
    REQUIRE(out[0].message["transport"]["time_sig"] == json::array({7, 8}));
    REQUIRE(f.session->pattern().numSteps() == 2 * 7 * 4);
    REQUIRE(f.engine.transport().numerator() == 7);
    REQUIRE(f.reply({{"type", "transport"}, {"time_sig", {7, 3}}})["code"] == "bad_request");
    REQUIRE(f.reply({{"type", "transport"}, {"time_sig", "7/4"}})["code"] == "bad_request");

    // get / set pattern
    r = f.reply({{"type", "get_pattern"}, {"id", 2}});
    REQUIRE(r["type"] == "get_pattern_ok");
    REQUIRE(r["edited"] == true);
    json pat = r["pattern"];
    REQUIRE(pat["tracks"][0]["steps"].is_array());
    pat["tracks"][0]["steps"][1] = 77;
    pat["time_sig"] = {4, 4};
    out = f.send({{"type", "set_pattern"}, {"id", 3}, {"pattern", pat}});
    REQUIRE(out.size() == 3); // reply, pattern event to others, state (meter changed) to all
    REQUIRE(out[0].message["type"] == "set_pattern_ok");
    REQUIRE(out[1].target == Outgoing::Target::Others);
    REQUIRE(out[1].message["type"] == "pattern");
    REQUIRE(out[1].message["pattern"]["tracks"][0]["steps"][1] == 77);
    REQUIRE(f.session->pattern().tracks[0].steps[1] == 77);
    REQUIRE(f.reply({{"type", "set_pattern"}, {"pattern", 5}})["code"] == "bad_request");
    // another file without changing the current one
    r = f.reply({{"type", "get_pattern"}, {"path", "presets/patterns/bossa-nova.json"}});
    REQUIRE(r["pattern"]["name"] == "Bossa Nova");
    REQUIRE(f.session->patternPath() == "presets/patterns/du-hast-stomp.json");

    // save to userdata, then it is listed
    r = f.reply({{"type", "save_pattern"}, {"name", "My Groove"}});
    REQUIRE(r["type"] == "save_pattern_ok");
    REQUIRE(r["path"] == "userdata/patterns/my-groove.json");
    REQUIRE(f.reply({{"type", "save_pattern"}, {"name", "My Groove"}})["code"] == "exists");
    bool listed = false;
    const json listing = f.reply({{"type", "list_patterns"}});
    INFO(listing.dump());
    for (const auto& p : listing["patterns"])
        listed = listed || (p["path"] == "userdata/patterns/my-groove.json" && p["factory"] == false);
    REQUIRE(listed);

    // kit params via set_param on the rhythm node
    f.send({{"type", "set_param"}, {"node", kit}, {"param", "snare_tune"}, {"value", 3}});
    REQUIRE(f.session->model().patch().rhythm.drums.params.at("snare_tune") == 3.0f);

    // a preset without "rhythm" keeps the rhythm section and the tempo
    f.send({{"type", "transport"}, {"tempo", 99}});
    f.reply({{"type", "load_preset"}, {"path", "presets/factory/synth-lead/basic-saw-lead.json"}});
    REQUIRE(f.session->model().patch().rhythm.drums.params.at("snare_tune") == 3.0f);
    REQUIRE(f.session->patternPath() == "userdata/patterns/my-groove.json");
    // ...while a drum preset brings its own kit + pattern
    f.reply({{"type", "load_preset"}, {"path", "presets/factory/drums/linn-80s-kit.json"}});
    REQUIRE(f.session->model().patch().rhythm.drums.params.at("kit") == 2.0f);
    REQUIRE(f.session->patternPath() == "presets/patterns/billie-jean-groove.json");

    // empty pattern
    f.send({{"type", "transport"}, {"pattern", ""}});
    REQUIRE(f.session->patternPath().empty());
    REQUIRE(f.session->pattern().tracks.size() == 8);

    // telemetry carries the step
    const json tele = f.session->pollTelemetry();
    REQUIRE(tele["transport"].contains("step"));
    REQUIRE(tele["transport"].contains("bar"));
}

TEST_CASE("protocol: garbage input never crashes", "[protocol]") {
    Fixture f;
    REQUIRE(f.handler->handleText("{not json")[0].message["code"] == "parse_error");
    REQUIRE(f.handler->handleText("[1,2]")[0].message["code"] == "bad_request");
    REQUIRE(f.handler->handleText(R"({"type":"wat"})")[0].message["code"] == "unknown_type");
    REQUIRE(f.handler->handleText(R"({"id":3})")[0].message["id"] == 3);
    for (const char* t : {"set_param", "load_preset", "save_preset", "set_patch", "add_layer", "remove_layer", "set_zone",
                          "set_instrument", "add_fx", "remove_fx", "move_fx", "set_fx_bypass", "note", "cc", "transport",
                          "set_audio_device", "open_audio_panel", "list_patterns", "get_pattern", "set_pattern",
                          "save_pattern"}) {
        for (const json& payload : {json::object(), json{{"node", -1}, {"layer", "x"}, {"zone", 5}, {"patch", 1},
                                                         {"value", nullptr}, {"note", 1e9}, {"to", "a"},
                                                         {"pattern", json::array()}, {"time_sig", {0, 0}}, {"name", 3}}}) {
            json m = payload;
            m["type"] = t;
            REQUIRE_NOTHROW(f.handler->handle(m));
        }
    }
}

TEST_CASE("control server: origin, ws path and static path rules", "[protocol]") {
    REQUIRE(ControlServer::originAllowed("", 7341, 7340));
    REQUIRE(ControlServer::originAllowed("http://localhost:5173", 7341, 7340));
    REQUIRE(ControlServer::originAllowed("http://127.0.0.1:7340", 7341, 7340));
    REQUIRE(ControlServer::originAllowed("http://127.0.0.1:7341", 7341, 7340));
    REQUIRE_FALSE(ControlServer::originAllowed("http://evil.com", 7341, 7340));
    REQUIRE_FALSE(ControlServer::originAllowed("http://localhost.evil.com:5173", 7341, 7340));
    REQUIRE_FALSE(ControlServer::originAllowed("http://localhost:8080", 7341, 7340));
    REQUIRE_FALSE(ControlServer::originAllowed("null", 7341, 7340));
    REQUIRE(ControlServer::wsPathAllowed("/"));
    REQUIRE(ControlServer::wsPathAllowed("/ws?x=1"));
    REQUIRE_FALSE(ControlServer::wsPathAllowed("/admin"));

    const auto dist = std::filesystem::temp_directory_path() / "ks-dist-test";
    std::filesystem::create_directories(dist / "assets");
    { std::ofstream(dist / "index.html") << "<html></html>"; }
    { std::ofstream(dist / "assets" / "a.js") << "x"; }
    REQUIRE(ControlServer::resolveStatic("/", dist) == dist / "index.html");
    REQUIRE(ControlServer::resolveStatic("/assets/a.js?v=1", dist) == (dist / "assets" / "a.js").lexically_normal());
    REQUIRE(ControlServer::resolveStatic("/some/route", dist) == dist / "index.html");
    REQUIRE_FALSE(ControlServer::resolveStatic("/../secret.txt", dist));
    REQUIRE_FALSE(ControlServer::resolveStatic("/%2e%2e/%2e%2e/x.txt", dist));
    REQUIRE_FALSE(ControlServer::resolveStatic("/missing.js", dist));
    std::filesystem::remove_all(dist);
}
