#include "core/AppPaths.h"
#include "core/GraphBuilder.h"
#include "core/ModuleRegistry.h"
#include "core/PatchModel.h"
#include "dsp/NoteDivision.h"
#include "preset/PatchJson.h"
#include "preset/PresetStore.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <filesystem>
#include <set>

using namespace ks;
using nlohmann::json;

namespace {
std::set<NodeId> allIds(const Patch& p) {
    std::set<NodeId> s;
    for (const auto& l : p.layers) {
        s.insert(l.node);
        s.insert(l.instrument.node);
        for (const auto& f : l.fx) s.insert(f.node);
    }
    for (const auto& f : p.master.fx) s.insert(f.node);
    return s;
}
size_t idCount(const Patch& p) {
    size_t n = 0;
    for (const auto& l : p.layers) n += 2 + l.fx.size();
    return n + p.master.fx.size();
}
} // namespace

TEST_CASE("Patch JSON: lenient load, node ids, defaults", "[patch]") {
    const json j = json::parse(R"({
      "format": 1,
      "meta": { "name": "T", "category": "Synth Lead", "tags": ["a", 3], "bogus": 1 },
      "layers": [
        { "zone": { "key_lo": 200, "key_hi": 60, "volume_db": "loud" },
          "instrument": { "type": "basic", "params": { "cutoff": 99999, "nonexistent": 1 } },
          "fx": [ { "type": "gain", "node": 7 }, { "type": "no_such_fx" }, 5 ] },
        { "node": 7, "instrument": { "type": "basic" } }
      ],
      "master": { "volume_db": -3, "fx": [ { "type": "limiter" } ] },
      "unknownTop": true
    })");
    std::vector<std::string> w;
    Patch p = patchFromJson(j, &w);
    REQUIRE(p.layers.size() == 2);
    REQUIRE(p.meta.tags.size() == 1);
    PatchModel m(defaultRegistry());
    auto w2 = m.setPatch(p);
    w.insert(w.end(), w2.begin(), w2.end());
    REQUIRE(!w.empty());
    const Patch& q = m.patch();
    // Unique, non-zero ids for every slot (duplicate 7 reassigned).
    const auto ids = allIds(q);
    REQUIRE(ids.size() == idCount(q));
    REQUIRE(ids.count(0) == 0);
    // Params normalized: clamped, defaults filled, unknown dropped.
    const auto& ip = q.layers[0].instrument.params;
    REQUIRE(ip.at("cutoff") == 20000.0f);
    REQUIRE(ip.count("nonexistent") == 0);
    REQUIRE(ip.count("release") == 1);
    REQUIRE(q.layers[0].zone.keyLo == 127);
    REQUIRE(q.layers[0].zone.volumeDb == 0.0f);
    REQUIRE(q.layers[0].fx.size() == 2); // unknown fx kept (not loaded), non-object dropped
    REQUIRE(q.layers[0].fx[1].type == "no_such_fx");

    // Graph builder skips unknown modules with a warning, never crashes.
    auto br = GraphBuilder::build(q, defaultRegistry(), nullptr, 48000.0, 64);
    REQUIRE(br.graph);
    REQUIRE_FALSE(br.warnings.empty());
}

TEST_CASE("Migration: format 1 tempo-sync params -> shared NoteDivision enum", "[patch][preset]") {
    auto div = [](const char* label) { return static_cast<float>(dsp::noteDivisionIndex(label)); };
    const json v1 = json::parse(R"({
      "format": 1,
      "layers": [
        { "instrument": { "type": "va", "params": { "lfo1_sync": true, "lfo1_division": 7,
                                                     "lfo2_sync": 0, "lfo2_division": 3 } },
          "fx": [ { "type": "tremolo", "params": { "sync": true, "division": 6 } },
                  { "type": "delay", "params": { "sync": 10 } },
                  { "type": "phaser", "params": { "sync": 0 } },
                  { "type": "flanger", "params": { "sync": 6 } } ] },
        { "instrument": { "type": "va", "params": { "lfo2_sync": 1 } },
          "fx": [ { "type": "tremolo", "params": { "sync": false, "division": 2 } } ] }
      ],
      "master": { "fx": [ { "type": "tremolo", "params": { "sync": 1 } } ] }
    })");
    std::vector<std::string> w;
    PatchModel m(defaultRegistry());
    auto w2 = m.setPatch(patchFromJson(v1, &w));
    w.insert(w.end(), w2.begin(), w2.end());
    for (const auto& s : w) INFO(s);
    REQUIRE(w.empty()); // old ids consumed, nothing unknown
    const Patch& p = m.patch();
    const auto& va = p.layers[0].instrument.params;
    REQUIRE(va.at("lfo1_sync") == div("1/8D")); // old VA index 7 = 1/8 dotted
    REQUIRE(va.at("lfo2_sync") == 0.0f);        // sync off -> Off
    REQUIRE(va.count("lfo1_division") == 0);
    REQUIRE(p.layers[0].fx[0].params.at("sync") == div("1/8.")); // old tremolo index 6
    REQUIRE(p.layers[0].fx[1].params.at("sync") == div("1/8.")); // delay: old 10 (1/8.) shifted by the new 4/1
    REQUIRE(p.layers[0].fx[2].params.at("sync") == 0.0f);
    REQUIRE(p.layers[0].fx[3].params.at("sync") == div("1/4"));
    REQUIRE(p.layers[1].instrument.params.at("lfo2_sync") == div("1/4")); // on, old default division (1/4)
    REQUIRE(p.layers[1].fx[0].params.at("sync") == 0.0f);
    REQUIRE(p.master.fx[0].params.at("sync") == div("1/8")); // on, old tremolo default (1/8)

    // Written back as format 2; loading that again changes nothing (no double shift).
    const json v2 = patchToJson(p);
    REQUIRE(v2.at("format") == kPatchFormat);
    PatchModel m2(defaultRegistry());
    REQUIRE(m2.setPatch(patchFromJson(v2)).empty());
    REQUIRE(patchToJson(m2.patch()) == v2);

    // Current format with a hand-written old `division` id: still merged.
    json mixed = v2;
    mixed["layers"][1]["fx"][0]["params"] = {{"sync", 1}, {"division", 4}};
    PatchModel m3(defaultRegistry());
    REQUIRE(m3.setPatch(patchFromJson(mixed)).empty());
    REQUIRE(m3.patch().layers[1].fx[0].params.at("sync") == div("1/4"));
    // Current format: delay index kept as is.
    REQUIRE(m3.patch().layers[0].fx[1].params.at("sync") == div("1/8."));
}

TEST_CASE("Migration: every old division maps, edge cases", "[patch][preset]") {
    auto load = [](const json& j) {
        PatchModel m(defaultRegistry());
        m.setPatch(patchFromJson(j));
        return m.patch();
    };
    auto patchWith = [](int format, const char* type, json params, bool instrument) {
        json slot = {{"type", type}, {"params", std::move(params)}};
        json layer = instrument ? json{{"instrument", slot}} : json{{"instrument", {{"type", "basic"}}}, {"fx", {slot}}};
        return json{{"format", format}, {"layers", {layer}}};
    };
    std::set<float> seen;
    for (int i = 0; i < 12; ++i) { // old tremolo list: 12 distinct divisions, all valid
        const Patch p = load(patchWith(1, "tremolo", {{"sync", true}, {"division", i}}, false));
        const float v = p.layers[0].fx[0].params.at("sync");
        REQUIRE(v >= 1.0f);
        seen.insert(v);
    }
    REQUIRE(seen.size() == 12);
    seen.clear();
    for (int i = 0; i < 15; ++i) { // old VA list: 15
        const Patch p = load(patchWith(1, "va", {{"lfo1_sync", 1}, {"lfo1_division", i}}, true));
        const float v = p.layers[0].instrument.params.at("lfo1_sync");
        REQUIRE(v >= 1.0f);
        seen.insert(v);
    }
    REQUIRE(seen.size() == 15);
    REQUIRE(seen.count(static_cast<float>(dsp::noteDivisionIndex("4/1"))) == 1);
    // Out-of-range old division -> old default; VA without any LFO ids keeps the default (Off).
    REQUIRE(load(patchWith(1, "tremolo", {{"sync", 1}, {"division", 99}}, false)).layers[0].fx[0].params.at("sync") ==
            static_cast<float>(dsp::noteDivisionIndex("1/8")));
    REQUIRE(load(patchWith(1, "va", json::object(), true)).layers[0].instrument.params.at("lfo2_sync") == 0.0f);
    // Format-1 delay sync index shifts by one (4/1 inserted).
    REQUIRE(load(patchWith(1, "delay", {{"sync", 5}}, false)).layers[0].fx[0].params.at("sync") == 6.0f);
    // Current format: an enum `sync` next to a stray old `division` stays as is (division dropped).
    REQUIRE(load(patchWith(kPatchFormat, "tremolo", {{"sync", 5}, {"division", 3}}, false))
                .layers[0]
                .fx[0]
                .params.at("sync") == 5.0f);
    // Layer volume clamp warns.
    json j = patchWith(kPatchFormat, "delay", json::object(), false);
    j["layers"][0]["zone"] = {{"volume_db", 40}};
    PatchModel m(defaultRegistry());
    const auto w = m.setPatch(patchFromJson(j));
    REQUIRE(m.patch().layers[0].zone.volumeDb == PatchModel::kMaxVolumeDb);
    REQUIRE(std::any_of(w.begin(), w.end(), [](const std::string& s) { return s.find("clamped") != std::string::npos; }));
}

TEST_CASE("Patch JSON round-trip is stable", "[patch]") {
    PatchModel m(defaultRegistry());
    const NodeId l2 = m.addLayer(std::nullopt, "basic");
    m.addFx(l2, "gain", std::nullopt);
    m.addFx(kMasterNode, "limiter", std::nullopt);
    Zone z = m.findLayer(l2)->zone;
    z.keyLo = 60;
    z.transpose = 12;
    z.pan = -0.5f;
    m.setZone(l2, z);
    m.setParam(m.findLayer(l2)->instrument.node, "cutoff", 1234.0f);
    const json j1 = patchToJson(m.patch());

    PatchModel m2(defaultRegistry());
    REQUIRE(m2.setPatch(patchFromJson(j1)).empty());
    const json j2 = patchToJson(m2.patch());
    REQUIRE(j1 == j2);
}

TEST_CASE("PatchModel edits and errors", "[patch]") {
    PatchModel m(defaultRegistry());
    REQUIRE(m.patch().layers.size() == 1);
    const NodeId layer = m.patch().layers[0].node;
    const NodeId fx = m.addFx(layer, "gain", std::nullopt);
    const NodeId fx2 = m.addFx(layer, "limiter", 0);
    REQUIRE(m.patch().layers[0].fx[0].node == fx2);
    m.moveFx(fx2, 5);
    REQUIRE(m.patch().layers[0].fx[1].node == fx2);
    REQUIRE(m.chainOf(fx) == layer);
    m.setFxBypass(fx, true);
    REQUIRE(m.findSlot(fx)->bypass);
    REQUIRE_THROWS_AS(m.addFx(layer, "basic", std::nullopt), PatchError);     // instrument as fx
    REQUIRE_THROWS_AS(m.setInstrument(layer, "gain"), PatchError);            // fx as instrument
    REQUIRE_THROWS_AS(m.setParam(fx, "nope", 1.0f), PatchError);
    REQUIRE_THROWS_AS(m.setParam(9999, "gain_db", 1.0f), PatchError);
    REQUIRE_THROWS_AS(m.setParam(fx2, "gr_db", 1.0f), PatchError);            // read-only
    REQUIRE(m.setParam(fx, "gain_db", 1000.0f) == 24.0f);
    m.removeFx(fx);
    REQUIRE(m.patch().layers[0].fx.size() == 1);
    const NodeId instr = m.patch().layers[0].instrument.node;
    m.setInstrument(layer, "basic");
    REQUIRE(m.patch().layers[0].instrument.node != instr); // new slot = new id
    m.removeLayer(layer);
    REQUIRE(m.patch().layers.empty());
    REQUIRE_THROWS_AS(m.removeLayer(layer), PatchError);
    REQUIRE(m.dirty());
}

TEST_CASE("GraphBuilder reuses unchanged modules", "[patch][graph]") {
    PatchModel m(defaultRegistry());
    const NodeId layer = m.patch().layers[0].node;
    m.addFx(layer, "gain", std::nullopt);
    auto a = GraphBuilder::build(m.patch(), defaultRegistry(), nullptr, 48000.0, 64);
    REQUIRE(a.created == 3); // instrument, gain, rhythm kit
    m.addFx(layer, "limiter", std::nullopt);
    auto b = GraphBuilder::build(m.patch(), defaultRegistry(), a.graph.get(), 48000.0, 64);
    REQUIRE(b.reused == 3);
    REQUIRE(b.created == 1);
    const NodeId instr = m.patch().layers[0].instrument.node;
    REQUIRE(a.graph->findModule(instr) == b.graph->findModule(instr));
    // Different sample rate: no reuse.
    auto c = GraphBuilder::build(m.patch(), defaultRegistry(), b.graph.get(), 44100.0, 64);
    REQUIRE(c.reused == 0);
    // State change: rebuilt.
    m.setModuleState(instr, json{{"x", 1}});
    auto d = GraphBuilder::build(m.patch(), defaultRegistry(), b.graph.get(), 48000.0, 64);
    REQUIRE(d.graph->findModule(instr) != b.graph->findModule(instr));
}

TEST_CASE("Path allow-list", "[paths]") {
    const AppPaths p = AppPaths::fromRoot(KS_SOURCE_DIR);
    REQUIRE(p.resolveAllowed("presets/factory/synth-lead/basic-saw-lead.json"));
    REQUIRE(p.resolveAllowed("userdata/presets/x.json"));
    REQUIRE_FALSE(p.resolveAllowed("presets/../engine/CMakeLists.txt"));
    REQUIRE_FALSE(p.resolveAllowed("C:/Windows/win.ini"));
    REQUIRE_FALSE(p.resolveAllowed("../outside.json"));
    REQUIRE_FALSE(p.resolveAllowed(""));
    REQUIRE_FALSE(p.resolveAllowed(R"(\\evil-host\share\x.json)"));
    REQUIRE_FALSE(p.resolveAllowed("//evil-host/share/x.json"));
    REQUIRE(PresetStore::slugify("My  Fancy/Preset!") == "my-fancy-preset");
}

TEST_CASE("PresetStore lists and loads factory presets", "[preset]") {
    const PresetStore store(AppPaths::fromRoot(KS_SOURCE_DIR));
    const auto list = store.list();
    int factory = 0;
    for (const auto& i : list) {
        if (!i.factory) continue;
        ++factory;
        std::vector<std::string> w;
        Patch p = store.load(i.path, &w);
#if !defined(KS_HAS_SFIZZ)
        // Built without sfizz (KS_WITH_SFIZZ=OFF): `sampler` presets reference an unregistered module.
        if (std::any_of(p.layers.begin(), p.layers.end(), [](const Layer& l) { return l.instrument.type == "sampler"; }))
            continue;
#endif
        PatchModel m(defaultRegistry());
        auto w2 = m.setPatch(p);
        INFO(i.path);
        REQUIRE(w.empty());
        REQUIRE(w2.empty());
        REQUIRE_FALSE(m.patch().layers.empty());
    }
    REQUIRE(factory >= 2);
    REQUIRE_THROWS_AS(store.load("engine/CMakeLists.txt"), PatchError);
}
