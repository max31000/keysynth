// Songs / setlists (ARCHITECTURE §18, PROTOCOL.md "Songs"): protocol, section switching, control keys, sync,
// factory songs; chord_learn (ARCHITECTURE §17).

#include "FakeCloud.h"

#include "cloud/SyncService.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "song/Song.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace ks;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct SongFixture {
    fs::path root;
    Engine engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<ProtocolHandler> handler;

    SongFixture() {
        root = fs::temp_directory_path() / ("ks-songs-" + std::to_string(std::random_device{}()));
        fs::remove_all(root);
        fs::create_directories(root / "presets");
        for (const char* d : {"factory", "patterns", "songs"})
            if (fs::exists(fs::path(KS_SOURCE_DIR) / "presets" / d))
                fs::copy(fs::path(KS_SOURCE_DIR) / "presets" / d, root / "presets" / d, fs::copy_options::recursive);
        engine.prepare(48000.0, 64);
        session = std::make_unique<Session>(engine, defaultRegistry(), AppPaths::fromRoot(root));
        session->rebuild(false);
        handler = std::make_unique<ProtocolHandler>(*session);
    }
    ~SongFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::vector<Outgoing> send(json j) { return handler->handle(j); }
    json reply(json j) {
        for (auto& o : send(std::move(j)))
            if (o.target == Outgoing::Target::Reply) return o.message;
        return nullptr;
    }
    static const json* find(const std::vector<Outgoing>& out, const std::string& type, Outgoing::Target t) {
        for (const auto& o : out)
            if (o.target == t && o.message.value("type", "") == type) return &o.message;
        return nullptr;
    }
    void block(std::vector<MidiEvent> ev = {}) {
        std::vector<float> l(64), r(64);
        AudioBlock ab{l.data(), r.data(), 64};
        engine.processBlock(ab, MidiEventSpan(ev.data(), ev.size()));
    }
};

const char* kEp = "presets/factory/e-piano/riders-on-the-storm-rhodes.json";
const char* kOrgan = "presets/factory/organ/vox-light-my-fire.json";

json demoSong() {
    return {{"title", "Test Song"},
            {"artist", "Band"},
            {"tempo", 100},
            {"time_sig", {3, 4}},
            {"notation", {{"musicxml", "userdata/scores/x.musicxml"}}}, // reserved: kept verbatim
            {"sections",
             {{{"name", "Intro"}, {"preset", kEp}, {"drums", false}},
              {{"name", "Verse"}, {"preset", kOrgan}, {"pattern", "presets/patterns/basic-rock.json"}, {"drums", true},
               {"tempo", 112}, {"bars", 16}},
              {{"name", "Outro"}, {"preset", kEp}}}}};
}

} // namespace

TEST_CASE("songs: save, list, get, delete; validation; reserved fields survive", "[songs][protocol]") {
    SongFixture f;
    auto out = f.send({{"type", "song_save"}, {"id", 1}, {"song", demoSong()}});
    const json* ok = SongFixture::find(out, "song_save_ok", Outgoing::Target::Reply);
    REQUIRE(ok);
    CHECK((*ok)["path"] == "userdata/songs/test-song.json");
    CHECK(SongFixture::find(out, "songs_changed", Outgoing::Target::Others));
    REQUIRE(fs::exists(f.root / "userdata" / "songs" / "test-song.json"));
    // exists without overwrite
    CHECK(f.reply({{"type", "song_save"}, {"song", demoSong()}})["code"] == "exists");

    const json list = f.reply({{"type", "song_list"}});
    bool found = false;
    for (const auto& s : list["songs"])
        if (s["path"] == "userdata/songs/test-song.json") {
            found = true;
            CHECK(s["sections"] == 3);
            CHECK(s["factory"] == false);
            CHECK(s["id"] == "test-song");
        }
    CHECK(found);

    const json got = f.reply({{"type", "song_get"}, {"path", "userdata/songs/test-song.json"}});
    REQUIRE(got["type"] == "song_get_ok");
    CHECK(got["song"]["id"] == "test-song");
    CHECK(got["song"]["notation"]["musicxml"] == "userdata/scores/x.musicxml");
    CHECK(got["song"]["sections"][1]["bars"] == 16);
    CHECK(got["song"]["time_sig"] == json({3, 4}));

    // validation
    json bad = demoSong();
    bad["sections"][0]["preset"] = "C:/evil.json";
    CHECK(f.reply({{"type", "song_save"}, {"song", bad}, {"overwrite", true}})["code"] == "bad_request");
    bad = demoSong();
    bad["sections"] = json::array();
    CHECK(f.reply({{"type", "song_save"}, {"song", bad}, {"overwrite", true}})["code"] == "bad_request");
    bad = demoSong();
    bad["sections"][1]["pattern"] = "../x.json";
    CHECK(f.reply({{"type", "song_save"}, {"song", bad}, {"overwrite", true}})["code"] == "bad_request");
    // a bare preset slug is a user preset reference
    json slug = demoSong();
    slug["sections"][2]["preset"] = "warm-rhodes";
    CHECK(f.reply({{"type", "song_save"}, {"song", slug}, {"overwrite", true}})["type"] == "song_save_ok");
    CHECK(presetRefToPath("warm-rhodes") == "userdata/presets/warm-rhodes.json");
    CHECK(presetRefToPath("../x").empty());

    CHECK(f.reply({{"type", "song_delete"}, {"path", "presets/songs/x.json"}})["code"] == "read_only");
    out = f.send({{"type", "song_delete"}, {"path", "userdata/songs/test-song.json"}});
    CHECK(SongFixture::find(out, "song_delete_ok", Outgoing::Target::Reply));
    CHECK_FALSE(fs::exists(f.root / "userdata" / "songs" / "test-song.json"));
    CHECK(f.reply({{"type", "song_delete"}, {"path", "userdata/songs/test-song.json"}})["code"] == "not_found");
}

TEST_CASE("songs: select, sections apply preset/tempo/drums/pattern, next/prev, close", "[songs][protocol]") {
    SongFixture f;
    REQUIRE(f.reply({{"type", "song_save"}, {"song", demoSong()}})["type"] == "song_save_ok");
    auto out = f.send({{"type", "song_select"}, {"id", 3}, {"path", "userdata/songs/test-song.json"}});
    REQUIRE(SongFixture::find(out, "song_select_ok", Outgoing::Target::Reply));
    const json* st = SongFixture::find(out, "song_state", Outgoing::Target::Broadcast);
    REQUIRE(st);
    CHECK((*st)["section"] == 0);
    CHECK((*st)["path"] == "userdata/songs/test-song.json");
    CHECK((*st)["song"]["title"] == "Test Song");
    CHECK(SongFixture::find(out, "state", Outgoing::Target::Broadcast));
    CHECK(f.session->presetPath() == kEp);
    CHECK(f.engine.transport().tempo() == 100.0);
    CHECK(f.engine.transport().numerator() == 3);
    CHECK_FALSE(f.engine.transport().playing());

    out = f.send({{"type", "song_next"}});
    CHECK(SongFixture::find(out, "song_state", Outgoing::Target::Broadcast));
    CHECK(f.session->songSection() == 1);
    CHECK(f.session->presetPath() == kOrgan);
    CHECK(f.engine.transport().tempo() == 112.0);
    CHECK(f.engine.transport().playing());
    CHECK(f.session->patternPath() == "presets/patterns/basic-rock.json");
    CHECK_FALSE(f.session->patternEdited()); // the song meter does not mark the pattern edited

    f.send({{"type", "song_section"}, {"index", 2}});
    CHECK(f.session->songSection() == 2);
    CHECK(f.engine.transport().tempo() == 100.0); // back to the song tempo
    out = f.send({{"type", "song_next"}}); // at the end: no change, no broadcast
    CHECK_FALSE(SongFixture::find(out, "song_state", Outgoing::Target::Broadcast));
    CHECK(f.session->songSection() == 2);
    CHECK(f.reply({{"type", "song_section"}, {"index", 9}})["code"] == "bad_request");
    f.send({{"type", "song_prev"}});
    CHECK(f.session->songSection() == 1);

    // A missing preset is a warning; the other steps still apply.
    json s = demoSong();
    s["sections"][0]["preset"] = "presets/factory/organ/does-not-exist.json";
    s["sections"][0]["tempo"] = 90;
    f.reply({{"type", "song_save"}, {"song", s}, {"overwrite", true}}); // active song reloaded
    const json r = f.reply({{"type", "song_section"}, {"index", 0}});
    REQUIRE(r["type"] == "song_section_ok");
    CHECK(r["warnings"].size() == 1);
    CHECK(f.engine.transport().tempo() == 90.0);

    out = f.send({{"type", "song_close"}});
    st = SongFixture::find(out, "song_state", Outgoing::Target::Broadcast);
    REQUIRE(st);
    CHECK((*st)["path"].is_null());
    CHECK((*st)["section"] == -1);
    CHECK(f.reply({{"type", "song_next"}})["code"] == "no_song");

    // hello carries song_state after sync_status
    out = f.send({{"type", "hello"}, {"id", 1}});
    REQUIRE(out.size() >= 3);
    CHECK(out[1].message["type"] == "sync_status");
    CHECK(out[2].message["type"] == "song_state");
}

TEST_CASE("songs: control keys switch sections from the keyboard and never sound", "[songs]") {
    SongFixture f;
    REQUIRE(f.reply({{"type", "song_save"}, {"song", demoSong()}})["type"] == "song_save_ok");
    // Disabled by default: notes pass.
    CHECK_FALSE(f.session->globalControlKeys().enabled);
    REQUIRE(f.reply({{"type", "song_control_keys"}, {"enabled", true}, {"prev", 21}, {"next", 23}})["type"] ==
            "song_control_keys_ok");
    // No song selected: keys are inactive.
    f.block({MidiEvent::noteOn(23, 100)});
    CHECK_FALSE(f.session->pollSongKeys());
    f.session->pollMidi();

    f.send({{"type", "song_select"}, {"path", "userdata/songs/test-song.json"}});
    REQUIRE(f.session->songSection() == 0);
    f.block({MidiEvent::noteOn(23, 100), MidiEvent::noteOff(23)});
    const json midi = f.session->pollMidi();
    if (!midi.is_null())
        for (const auto& n : midi["notes"]) CHECK_FALSE((n["note"] == 23 && n["on"] == true));
    CHECK(f.session->pollSongKeys());
    CHECK(f.session->songSection() == 1);
    // Two presses in one tick -> one switch to the resulting section.
    f.block({MidiEvent::noteOn(21, 100), MidiEvent::noteOff(21), MidiEvent::noteOn(21, 100)});
    CHECK(f.session->pollSongKeys());
    CHECK(f.session->songSection() == 0);
    // Other keys still play.
    f.block({MidiEvent::noteOn(60, 100)});
    CHECK_FALSE(f.session->pollSongKeys());

    // Per-song override.
    json s = demoSong();
    s["control_keys"] = {{"enabled", true}, {"prev", 30}, {"next", 31}};
    f.reply({{"type", "song_save"}, {"song", s}, {"overwrite", true}});
    const json st = f.session->songStateJson();
    CHECK(st["controlKeys"]["source"] == "song");
    CHECK(st["controlKeys"]["next"] == 31);
    CHECK(st["globalControlKeys"]["next"] == 23);
    f.block({MidiEvent::noteOn(31, 100)});
    CHECK(f.session->pollSongKeys());
    CHECK(f.session->songSection() == 1);

    // Persisted in settings.json; a new session reads it.
    Session again(f.engine, defaultRegistry(), AppPaths::fromRoot(f.root));
    CHECK(again.globalControlKeys().enabled);
    CHECK(again.globalControlKeys().next == 23);
    CHECK(f.reply({{"type", "song_control_keys"}, {"prev", 200}})["code"] == "bad_request");
}

TEST_CASE("songs: factory songs are valid and reference existing presets and patterns", "[songs][presets]") {
    const AppPaths paths = AppPaths::fromRoot(KS_SOURCE_DIR);
    const auto list = songs::list(paths);
    int factory = 0;
    for (const auto& info : list) {
        if (!info.factory) continue;
        ++factory;
        INFO(info.path);
        std::vector<std::string> w;
        const Song s = songs::load(paths, info.path, &w);
        CHECK(w.empty());
        CHECK(validateSong(s).empty());
        CHECK(s.id == info.id);
        for (const auto& sec : s.sections) {
            INFO(sec.name);
            if (sec.preset) {
                const std::string p = presetRefToPath(*sec.preset);
                REQUIRE(p.rfind("presets/factory/", 0) == 0); // factory songs only use factory presets
                CHECK(fs::exists(fs::path(KS_SOURCE_DIR) / p));
            }
            if (sec.pattern && !sec.pattern->empty()) CHECK(fs::exists(fs::path(KS_SOURCE_DIR) / *sec.pattern));
        }
    }
    CHECK(factory >= 4);
}

TEST_CASE("chord_learn copies held notes into the chord memory", "[songs][midi][protocol]") {
    SongFixture f;
    const NodeId layer = f.session->model().patch().layers.at(0).node;
    const json st = f.reply({{"type", "add_fx"}, {"layer", layer}, {"module", "chord"}});
    REQUIRE(st["type"] == "state");
    const json& midi = st["patch"]["layers"][0]["midi"];
    REQUIRE(midi.size() == 1);
    const NodeId node = midi[0]["node"].get<NodeId>();
    f.block();
    CHECK(f.reply({{"type", "chord_learn"}, {"node", node}})["code"] == "no_notes");
    const NodeId inst = f.session->model().patch().layers.at(0).instrument.node;
    CHECK(f.reply({{"type", "chord_learn"}, {"node", inst}})["code"] == "bad_request");
    CHECK(f.reply({{"type", "chord_learn"}, {"node", 9999}})["code"] == "not_found");
    f.block({MidiEvent::noteOn(62, 100), MidiEvent::noteOn(65, 100), MidiEvent::noteOn(69, 100), MidiEvent::noteOn(72, 100)});
    const auto out = f.send({{"type", "chord_learn"}, {"id", 4}, {"node", node}});
    const json* ok = SongFixture::find(out, "chord_learn_ok", Outgoing::Target::Reply);
    REQUIRE(ok);
    CHECK((*ok)["intervals"] == json({0, 3, 7, 10}));
    CHECK(SongFixture::find(out, "param", Outgoing::Target::Broadcast));
    const ModuleSlot* slot = f.session->model().findSlot(node);
    REQUIRE(slot);
    CHECK(slot->params.at("mem_count") == 4.0f);
    CHECK(slot->params.at("mem_4") == 10.0f);
    CHECK(slot->params.at("type") == 12.0f); // Memory
    CHECK(f.session->model().dirty());
    // the patch JSON round-trips the midi chain
    const json p = f.reply({{"type", "get_patch"}});
    CHECK(p["patch"]["layers"][0]["midi"][0]["type"] == "chord");
}

TEST_CASE("sync: songs use /api/songs and their own sync.json section", "[songs][cloud][sync]") {
    const fs::path root = fs::temp_directory_path() / ("ks-songsync-" + std::to_string(std::random_device{}()));
    fs::remove_all(root);
    fs::create_directories(root / "presets");
    fs::create_directories(root / "songs");
    auto fake = std::make_shared<test::FakeCloud>();
    std::mutex mu;
    auto api = [&](cloud::HttpRequest r) {
        r.url = test::kFakeBase + r.url;
        r.headers.emplace_back("Authorization", "Bearer " + test::kFakeToken);
        return fake->send(r, nullptr, nullptr);
    };
    std::ofstream(root / "presets" / "p1.json", std::ios::binary) << json({{"format", 2}}).dump(2) << "\n";
    std::ofstream(root / "songs" / "my-song.json", std::ios::binary) << json({{"format", 1}, {"title", "Mine"}}).dump(2) << "\n";
    fake->songs["server-song"] = {3, false, json({{"format", 1}, {"title", "From server"}})};

    cloud::SyncEngine presets(api, root / "presets", root / "sync.json", &mu);
    REQUIRE(presets.runPass().ok);
    cloud::SyncEngine songsEngine(api, root / "songs", root / "sync.json", &mu, cloud::SyncCollection::songs());
    REQUIRE(songsEngine.runPass().ok);

    CHECK(fake->presets.count("p1") == 1);
    CHECK(fake->presets.count("my-song") == 0);
    REQUIRE(fake->songs.count("my-song") == 1);
    CHECK(fake->songs["my-song"].patch["title"] == "Mine");
    REQUIRE(fs::exists(root / "songs" / "server-song.json"));
    std::ifstream sf(root / "sync.json");
    const json sync = json::parse(sf);
    CHECK(sync["presets"].contains("p1"));
    CHECK(sync["songs"].contains("my-song"));
    CHECK(sync["songs"]["server-song"]["baseRevision"] == 3);
    // conflict files are reported under userdata/songs/
    std::ofstream(root / "songs" / "x.conflict-20261013T100000Z.json") << "{}";
    const auto c = cloud::SyncEngine::conflicts(root / "songs", cloud::SyncCollection::songs().logicalDir);
    REQUIRE(c.size() == 1);
    CHECK(c[0].file == "userdata/songs/x.conflict-20261013T100000Z.json");
    sf.close();
    // An API without the songs collection (404 on the list) is not an error.
    cloud::SyncCollection missing = cloud::SyncCollection::songs();
    missing.endpoint = "/api/no-such-collection";
    CHECK(cloud::SyncEngine(api, root / "songs", root / "sync.json", &mu, missing).runPass().ok);
    std::error_code ec;
    fs::remove_all(root, ec);
}
