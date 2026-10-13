// Cloud basics: SHA-256, manifest path rules (fuzz), AppPaths modes + logical paths, DPAPI, cloud.json, session
// trust (Origin / Host / web allow-list), hello additions, cloud_link / unlink through CloudServices (fake API).

#include "FakeCloud.h"

#include "cloud/CloudClient.h"
#include "cloud/CloudServices.h"
#include "control/ControlServer.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "control/Trust.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/Sha256.h"
#include "platform/Dpapi.h"
#include "samples/SamplePathRules.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace ks;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
fs::path tempDir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path() / ("ks-cloud-" + tag + "-" + std::to_string(std::random_device{}()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
} // namespace

TEST_CASE("sha256: known vectors", "[cloud]") {
    CHECK(Sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Incremental == one shot, across block boundaries.
    std::string big(1000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 7);
    Sha256 h;
    for (size_t i = 0; i < big.size(); i += 37) h.update(big.data() + i, std::min<size_t>(37, big.size() - i));
    CHECK(h.hexDigest() == Sha256::hex(big));
    Sha256 m;
    for (int i = 0; i < 1000; ++i) m.update(std::string(1000, 'a'));
    CHECK(m.hexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sample paths: manifest path validation", "[cloud][paths]") {
    using samples::isValidManifestPath;
    for (const char* ok : {"a.sfz", "Dir/Sub Dir/A0v1.flac", "Салон/Пиано.wav", "x#1/y+z%.wav", "COMX/a.wav", "con-x.wav",
                           "a..b/c.wav", "LPT0.wav"})
        CHECK(isValidManifestPath(ok));
    for (const char* bad :
         {"", "/abs.wav", "../x.wav", "a/../../x.wav", "a/./b.wav", "./a.wav", "a//b.wav", "a/", "C:/x.wav", "C:x.wav",
          "a\\b.wav", "file.wav:ads", "file.wav::$DATA", "CON", "con.txt", "Aux.wav", "nul", "COM1.wav", "lpt9.sfz",
          "dir/PRN/x.wav", "trail./x.wav", "trail /x.wav", "x.wav.", "x.wav ", "bell\x07.wav", "del\x7f.wav",
          "tab\t.wav", "q?.wav", "star*.wav", "pipe|.wav", "lt<.wav", "quote\".wav", "CON .txt", "COM\xC2\xB9.wav"})
        CHECK_FALSE(isValidManifestPath(bad));
    CHECK_FALSE(isValidManifestPath(std::string(241, 'a')));
    CHECK(isValidManifestPath(std::string(240, 'a')));
    CHECK_FALSE(isValidManifestPath("\xC0\xAF.wav"));      // overlong '/'
    CHECK_FALSE(isValidManifestPath("\xED\xA0\x80.wav"));  // surrogate
    CHECK_FALSE(isValidManifestPath("\xE2\x82.wav"));      // truncated
    CHECK(samples::isValidLibraryId("salamander"));
    CHECK(samples::isValidLibraryId("vpo-2"));
    for (const char* bad : {"", "-x", "X", "a_b", "a/b", "..", "a.b"}) CHECK_FALSE(samples::isValidLibraryId(bad));
}

TEST_CASE("sample paths: fuzzed paths never escape the cache", "[cloud][paths]") {
    const fs::path root = tempDir("fuzz");
    std::mt19937 rng(1234);
    const std::string alphabet[] = {"a", "B", ".", "..", "/", "\\", ":", " ", "%2e", "\xD0\x96", "CON", "\x01", "~", "#", "?"};
    for (int i = 0; i < 20000; ++i) {
        std::string p;
        const int n = 1 + static_cast<int>(rng() % 10);
        for (int k = 0; k < n; ++k) p += alphabet[rng() % std::size(alphabet)];
        const auto got = samples::cachePathFor(root, "lib", p);
        if (!got) continue;
        REQUIRE(samples::isValidManifestPath(p));
        REQUIRE(isPathInsideLexical(*got, root / "lib"));
        REQUIRE(got->lexically_normal() != (root / "lib").lexically_normal());
    }
    CHECK_FALSE(samples::cachePathFor(root, "../x", "a.wav"));
    CHECK_FALSE(samples::cachePathFor(root, "lib", "../a.wav"));
    fs::remove_all(root);
}

TEST_CASE("app paths: dev, installed, KS_DATA_DIR, profiles, logical paths", "[cloud][paths]") {
    const fs::path root = tempDir("paths");
    fs::create_directories(root / "presets" / "factory");
    SECTION("dev layout = repo layout") {
        const AppPaths p = AppPaths::fromRoot(root);
        CHECK_FALSE(p.installed());
        CHECK(p.userdata == p.localProfile);
        CHECK(p.settingsFile.filename() == "settings.json");
        CHECK(p.settingsFile.parent_path() == p.localProfile);
        CHECK(p.sampleCache == p.localProfile / "samples");
        CHECK(p.cloudFile == p.localProfile / "cloud.json");
        CHECK(p.pluginBuildDir == p.root / "plugins" / ".build");
        CHECK(p.relativeToRoot(p.userPresets / "x.json") == "userdata/presets/x.json");
        CHECK(p.relativeToRoot(p.factoryPresets / "a" / "b.json") == "presets/factory/a/b.json");
        CHECK(p.relativeToRoot(p.sampleCache / "lib" / "x.sfz") == "assets/samples/lib/x.sfz");
    }
    SECTION("installed layout") {
        fs::create_directories(root / "app");
        std::ofstream(root / "app" / "installed.flag") << "";
#if defined(_MSC_VER)
        _putenv_s("KS_DATA_DIR", pathToUtf8(root / "data").c_str());
#else
        setenv("KS_DATA_DIR", pathToUtf8(root / "data").c_str(), 1);
#endif
        AppPaths p = AppPaths::discover(root / "app");
#if defined(_MSC_VER)
        _putenv_s("KS_DATA_DIR", "");
#else
        unsetenv("KS_DATA_DIR");
#endif
        CHECK(p.installed());
        CHECK(p.dataRoot == fs::weakly_canonical(root / "data"));
        CHECK(p.localProfile == p.dataRoot / "userdata");
        CHECK(p.pluginsDir == p.dataRoot / "plugins");
        CHECK(p.pluginBuildDir == p.dataRoot / "faust-cache");
        CHECK(p.sampleCache == p.dataRoot / "samples");
        CHECK(p.logsDir == p.dataRoot / "logs");
        // Logical paths: userdata/ -> active profile, presets/ -> app dir.
        CHECK(p.resolveAllowed("userdata/presets/x.json") == p.localProfile / "presets" / "x.json");
        CHECK(p.resolveAllowed("presets/factory/a.json") == p.root / "presets" / "factory" / "a.json");
        CHECK(p.relativeToRoot(p.userPresets / "x.json") == "userdata/presets/x.json");
        REQUIRE(p.setProfile("17"));
        CHECK(p.userPresets == p.localProfile / "accounts" / "17" / "presets");
        CHECK(p.resolveAllowed("userdata/presets/x.json") == p.localProfile / "accounts" / "17" / "presets" / "x.json");
        CHECK(p.relativeToRoot(p.userPresets / "x.json") == "userdata/presets/x.json");
        CHECK_FALSE(p.setProfile("../evil"));
        CHECK_FALSE(p.resolveAllowed("C:/Windows/win.ini"));
        CHECK_FALSE(p.resolveAllowed("C:relative.json"));
        CHECK_FALSE(p.resolveAllowed("userdata/../../../outside.json"));
        CHECK(p.resolveAllowed(pathToUtf8(p.sampleCache / "lib" / "x.sfz")));
    }
    fs::remove_all(root);
}

#if defined(_WIN32) || defined(__APPLE__)
TEST_CASE("dpapi: round trip, wrong entropy, garbage", "[cloud]") {
    const std::string secret = test::kFakeToken;
    const auto blob = platform::dpapiProtect(secret, "keysynth-cloud-v1");
    REQUIRE(blob);
    CHECK(blob->find(secret) == std::string::npos);
    CHECK(platform::dpapiUnprotect(*blob, "keysynth-cloud-v1") == secret);
    CHECK_FALSE(platform::dpapiUnprotect(*blob, "other-entropy"));
    std::string corrupt = *blob;
    corrupt[corrupt.size() / 2] ^= 0x5a;
    CHECK_FALSE(platform::dpapiUnprotect(corrupt, "keysynth-cloud-v1"));
    CHECK_FALSE(platform::dpapiUnprotect("", "keysynth-cloud-v1"));
    const auto again = platform::dpapiProtect(secret, "keysynth-cloud-v1"); // fresh IV/salt every time
    REQUIRE(again);
    CHECK(*again != *blob);
    CHECK(platform::dpapiUnprotect(*again, "keysynth-cloud-v1") == secret);
    CHECK_FALSE(platform::computerName().empty());
}
#endif

TEST_CASE("cloud store: deviceId, DPAPI token round trip, undecryptable token", "[cloud]") {
    const fs::path d = tempDir("store");
    const fs::path file = d / "cloud.json";
    cloud::CloudStore s;
    s.load(file);
    REQUIRE(cloud::isUuidV4(s.deviceId));
    REQUIRE(fs::exists(file));
    const std::string id = s.deviceId;
    s.link = cloud::LinkInfo{"17", "Max", "2027-04-10T12:00:00Z"};
    s.token = test::kFakeToken;
    REQUIRE(s.save(file));
    {
        std::ifstream f(file);
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(ss.str().find(test::kFakeToken) == std::string::npos); // token never in clear text
    }
    cloud::CloudStore t;
    t.load(file);
    CHECK(t.deviceId == id);
    REQUIRE(t.link);
    CHECK(t.link->accountId == "17");
    CHECK(t.token == test::kFakeToken);
    // A blob from elsewhere (other user / corrupted) -> unlinked, deviceId kept.
    json j = json::parse(std::ifstream(file));
    j["link"]["token"] = "AAAAAAAA";
    std::ofstream(file) << j.dump();
    cloud::CloudStore u;
    u.load(file);
    CHECK(u.deviceId == id);
    CHECK_FALSE(u.link);
    CHECK(u.token.empty());
    fs::remove_all(d);
}

TEST_CASE("cloud: token format", "[cloud]") {
    CHECK(cloud::isValidToken(test::kFakeToken));
    CHECK(cloud::isValidToken("ksd_" + std::string(42, 'a') + "-"));
    CHECK_FALSE(cloud::isValidToken("ksd_" + std::string(42, 'a')));
    CHECK_FALSE(cloud::isValidToken("ksd_" + std::string(44, 'a')));
    CHECK_FALSE(cloud::isValidToken("ksx_" + std::string(43, 'a')));
    CHECK_FALSE(cloud::isValidToken("ksd_" + std::string(42, 'a') + "="));
}

// ---------------------------------------------------------------------------------------------------------------
// Trust

TEST_CASE("trust: Origin classification", "[cloud][protocol]") {
    OriginPolicy dev, inst;
    inst.devBuild = false;
    CHECK(classifyOrigin("", 7341, 7340, dev) == Trust::Local);
    CHECK(classifyOrigin("http://127.0.0.1:7340", 7341, 7340, inst) == Trust::Local);
    CHECK(classifyOrigin("http://localhost:7341", 7341, 7340, inst) == Trust::Local);
    CHECK(classifyOrigin("http://localhost:5173", 7341, 7340, dev) == Trust::Local);
    CHECK_FALSE(classifyOrigin("http://localhost:5173", 7341, 7340, inst));
    CHECK(classifyOrigin("https://mvv42.ru", 7341, 7340, inst) == Trust::Web);
    for (const char* bad : {"null", "https://www.mvv42.ru", "https://mvv42.ru:443", "https://mvv42.ru/", "http://mvv42.ru",
                            "https://mvv42.ru.evil.com", "https://evil.com", "http://localhost", "http://localhost:8080",
                            "http://localhost.evil.com:7340", "HTTP://LOCALHOST:7340", "http://127.0.0.1:7340/x"})
        CHECK_FALSE(classifyOrigin(bad, 7341, 7340, dev));
    OriginPolicy staging;
    staging.extraWebOrigin = "https://staging.example";
    CHECK(classifyOrigin("https://staging.example", 7341, 7340, staging) == Trust::Web);
    staging.devBuild = false;
    CHECK_FALSE(classifyOrigin("https://staging.example", 7341, 7340, staging));
}

TEST_CASE("trust: Host header (DNS rebinding)", "[cloud][protocol]") {
    CHECK(hostAllowed("127.0.0.1:7341", 7341));
    CHECK(hostAllowed("localhost:7341", 7341));
    CHECK(hostAllowed("LOCALHOST:7341", 7341));
    for (const char* bad : {"", "127.0.0.1", "localhost", "127.0.0.1:7340", "evil.com:7341", "127.0.0.1.evil.com:7341",
                            "localhost:7341.evil", "[::1]:7341", "0.0.0.0:7341"})
        CHECK_FALSE(hostAllowed(bad, 7341));
    CHECK(ControlServer::admit("127.0.0.1:7341", "https://mvv42.ru", 7341, 7341, 7340, OriginPolicy{}) == Trust::Web);
    CHECK_FALSE(ControlServer::admit("evil.com:7341", "", 7341, 7341, 7340, OriginPolicy{}));
    CHECK_FALSE(ControlServer::admit("127.0.0.1:7341", "null", 7341, 7341, 7340, OriginPolicy{}));
}

TEST_CASE("trust: web session allow-list per message", "[cloud][protocol]") {
    auto ok = [](const json& m) { return webRestriction(m).empty(); };
    for (const char* t : {"hello", "get_catalog", "set_param", "note", "cc", "panic", "save_preset", "save_pattern",
                          "cloud_link", "samples_fetch", "set_audio_device", "set_audio_input", "open_audio_panel",
                          "list_plugins"})
        CHECK(ok({{"type", t}}));
    for (const char* t : {"reload_plugin", "future_message", "", "set_root"}) CHECK_FALSE(ok({{"type", t}}));
    CHECK(ok({{"type", "load_preset"}, {"path", "presets/factory/piano/x.json"}}));
    CHECK(ok({{"type", "load_preset"}, {"path", "userdata/presets/x.json"}}));
    for (const char* bad : {"C:/x.json", "/etc/passwd", "presets/factory/../../x.json", "userdata/cloud.json",
                            "userdata\\presets\\x.json", "plugins/x/x.json", "presets/factory/", "assets/samples/x.json",
                            "presets/factory/x./a.json", "userdata/presets/a.json "})
        CHECK_FALSE(ok({{"type", "load_preset"}, {"path", bad}}));
    CHECK_FALSE(ok({{"type", "load_preset"}}));
    CHECK(ok({{"type", "get_pattern"}}));
    CHECK(ok({{"type", "get_pattern"}, {"path", "presets/patterns/a.json"}}));
    CHECK_FALSE(ok({{"type", "get_pattern"}, {"path", "C:/a.json"}}));
    CHECK(ok({{"type", "transport"}, {"playing", true}}));
    CHECK(ok({{"type", "transport"}, {"pattern", ""}}));
    CHECK(ok({{"type", "transport"}, {"pattern", "userdata/patterns/a.json"}}));
    CHECK_FALSE(ok({{"type", "transport"}, {"pattern", "../a.json"}}));
    CHECK_FALSE(ok({{"type", "transport"}, {"pattern", 5}}));
    const json goodPatch = {{"layers", {{{"instrument", {{"type", "sampler"}, {"state", {{"sfz", "assets/samples/a/b.sfz"}}}}}}}},
                            {"rhythm", {{"pattern", "presets/patterns/x.json"}}}};
    CHECK(ok({{"type", "set_patch"}, {"patch", goodPatch}}));
    json bad1 = goodPatch;
    bad1["layers"][0]["instrument"]["state"]["sfz"] = "C:/Windows/x.sfz";
    CHECK_FALSE(ok({{"type", "set_patch"}, {"patch", bad1}}));
    json bad2 = goodPatch;
    bad2["layers"][0]["fx"] = {{{"type", "x"}, {"state", {{"path", "../../x"}}}}};
    CHECK_FALSE(ok({{"type", "set_patch"}, {"patch", bad2}}));
    json bad3 = goodPatch;
    bad3["rhythm"]["pattern"] = "/abs.json";
    CHECK_FALSE(ok({{"type", "set_patch"}, {"patch", bad3}}));
    json bad4 = goodPatch;
    bad4["layers"][0]["instrument"] = {{"type", "fm"}, {"state", {{"syx", "\\\\host\\share\\x.syx"}}}};
    CHECK_FALSE(ok({{"type", "set_patch"}, {"patch", bad4}}));
    json bad5 = goodPatch;
    bad5["master"] = {{"fx", {{{"type", "y"}, {"state", {{"file", "D:/x"}}}}}}};
    CHECK_FALSE(ok({{"type", "set_patch"}, {"patch", bad5}}));
}

// ---------------------------------------------------------------------------------------------------------------
// Protocol + CloudServices with the fake API

namespace {
struct CloudFixture {
    fs::path root;
    Engine engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<ProtocolHandler> handler;
    std::shared_ptr<test::FakeCloud> fake = std::make_shared<test::FakeCloud>();
    std::unique_ptr<cloud::CloudServices> cloud;

    CloudFixture() {
        root = tempDir("svc");
        fs::create_directories(root / "presets" / "factory");
        fs::create_directories(root / "userdata" / "presets");
        std::ofstream(root / "userdata" / "presets" / "mine.json") << R"({"format": 2, "meta": {"name": "Mine"}})";
        engine.prepare(48000.0, 64);
        session = std::make_unique<Session>(engine, defaultRegistry(), AppPaths::fromRoot(root));
        session->rebuild(false);
        handler = std::make_unique<ProtocolHandler>(*session);
        cloud::CloudServices::Options o;
        o.baseUrl = test::kFakeBase;
        o.http = fake;
        o.syncIntervalMs = 3600000;
        o.sampleHook = false;
        o.retryBaseMs = 1;
        cloud = std::make_unique<cloud::CloudServices>(*session, o);
        handler->setCloud(cloud.get());
        cloud->start();
    }
    ~CloudFixture() {
        handler->setCloud(nullptr);
        cloud.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    json reply(const json& m, Trust trust = Trust::Local) {
        for (auto& o : handler->handle(m, ClientInfo{"c1", trust}))
            if (o.target == Outgoing::Target::Reply) return o.message;
        return nullptr;
    }
    // Poll until a reply with `id` (or timeout).
    json asyncReply(int id, int ms = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            for (auto& a : cloud->poll())
                if (a.out.target == Outgoing::Target::Reply && a.out.message.value("id", -1) == id) return a.out.message;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return nullptr;
    }
};
} // namespace

TEST_CASE("protocol: hello carries version, trust, cloud; then sync_status", "[cloud][protocol]") {
    CloudFixture f;
    const auto out = f.handler->handle({{"type", "hello"}, {"id", 1}}, ClientInfo{"c1", Trust::Web});
    REQUIRE(out.size() == 3); // state, sync_status, song_state
    const json& st = out[0].message;
    CHECK(st["type"] == "state");
    CHECK(st["id"] == 1);
    CHECK(st["version"] == engineVersion());
    CHECK(st["trust"] == "web");
    CHECK(st["cloud"]["linked"] == false);
    CHECK(cloud::isUuidV4(st["cloud"]["deviceId"].get<std::string>()));
    CHECK(st["cloud"]["accountId"].is_null());
    CHECK(out[1].message["type"] == "sync_status");
    CHECK(out[1].message["state"] == "disabled");
    CHECK_FALSE(out[1].message.contains("id"));
}

TEST_CASE("protocol: web sessions are default-deny", "[cloud][protocol]") {
    CloudFixture f;
    json r = f.reply({{"type", "reload_plugin"}, {"id", 3}, {"name", "x"}}, Trust::Web);
    CHECK(r["type"] == "error");
    CHECK(r["code"] == "forbidden");
    r = f.reply({{"type", "load_preset"}, {"id", 4}, {"path", "C:/Windows/win.ini"}}, Trust::Web);
    CHECK(r["code"] == "forbidden");
    r = f.reply({{"type", "list_presets"}, {"id", 5}}, Trust::Web);
    CHECK(r["type"] == "list_presets_ok");
    // local sessions keep the full protocol (no plugin host here -> not_available, not forbidden)
    r = f.reply({{"type", "reload_plugin"}, {"id", 6}, {"name", "x"}}, Trust::Local);
    CHECK(r["code"] == "not_available");
}

TEST_CASE("cloud: link, merge, account mismatch, unlink, 401 drop", "[cloud][protocol]") {
    CloudFixture f;
    CHECK(f.reply({{"type", "cloud_link"}, {"id", 1}, {"token", "nope"}})["code"] == "bad_request");

    REQUIRE(f.reply({{"type", "cloud_link"}, {"id", 2}, {"token", test::kFakeToken}}).is_null()); // async
    json r = f.asyncReply(2);
    REQUIRE(r["type"] == "cloud_status");
    CHECK(r["linked"] == true);
    CHECK(r["accountId"] == "17");
    CHECK(r["accountName"] == "Max");
    CHECK(r["merged"]["presets"] == 1);
    CHECK(f.session->paths().accountId == "17");
    CHECK(fs::exists(f.root / "userdata" / "accounts" / "17" / "presets" / "mine.json"));
    CHECK(f.cloud->client().status().linked);
    // cloud.json holds no clear-text token
    {
        std::ifstream cf(f.root / "userdata" / "cloud.json");
        std::stringstream ss;
        ss << cf.rdbuf();
        CHECK(ss.str().find(test::kFakeToken) == std::string::npos);
    }

    // Same account again: silent (no merged)
    f.reply({{"type", "cloud_link"}, {"id", 3}, {"token", test::kFakeToken}});
    r = f.asyncReply(3);
    CHECK(r["type"] == "cloud_status");
    CHECK_FALSE(r.contains("merged"));

    // Other account without replace -> account_mismatch, nothing changes
    f.reply({{"type", "cloud_link"}, {"id", 4}, {"token", test::kOtherToken}});
    r = f.asyncReply(4);
    CHECK(r["type"] == "error");
    CHECK(r["code"] == "account_mismatch");
    CHECK(f.session->paths().accountId == "17");
    f.reply({{"type", "cloud_link"}, {"id", 5}, {"token", test::kOtherToken}, {"replace", true}});
    r = f.asyncReply(5);
    CHECK(r["accountId"] == "42");
    CHECK(f.session->paths().accountId == "42");
    // A pass bound to the old token cannot continue after the re-link.
    CHECK(f.cloud->client().apiAs(test::kFakeToken, cloud::HttpRequest{"GET", "/api/presets", {}, {}, 1000}).error == "not_linked");

    // Invalid token -> token_invalid; offline -> offline
    f.reply({{"type", "cloud_link"}, {"id", 6}, {"token", "ksd_" + std::string(43, 'Z')}});
    CHECK(f.asyncReply(6)["code"] == "token_invalid");
    f.fake->offline = true;
    f.reply({{"type", "cloud_link"}, {"id", 7}, {"token", test::kFakeToken}});
    CHECK(f.asyncReply(7)["code"] == "offline");
    f.fake->offline = false;

    // Unlink -> local profile
    r = f.reply({{"type", "cloud_unlink"}, {"id", 8}});
    CHECK(r["type"] == "cloud_status");
    CHECK(r["linked"] == false);
    CHECK(f.session->paths().accountId.empty());
    CHECK(f.reply({{"type", "sync_now"}, {"id", 9}})["code"] == "not_linked");
    CHECK(f.reply({{"type", "samples_fetch"}, {"id", 10}, {"scope", "all"}})["code"] == "not_linked");

    // Link again, then the server revokes the token: next API call drops it and cloud_status says so.
    f.reply({{"type", "cloud_link"}, {"id", 11}, {"token", test::kFakeToken}});
    REQUIRE(f.asyncReply(11)["linked"] == true);
    {
        std::lock_guard<std::mutex> lk(f.fake->m);
        f.fake->tokens.erase(test::kFakeToken);
    }
    f.reply({{"type", "sync_now"}, {"id", 12}});
    bool dropped = false;
    for (int i = 0; i < 400 && !dropped; ++i) {
        for (auto& a : f.cloud->poll())
            if (a.out.message["type"] == "cloud_status" && a.out.message["linked"] == false) {
                dropped = true;
                CHECK(a.out.message["error"] == "token_invalid");
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(dropped);
    CHECK(f.session->paths().accountId.empty());
}

// Regression (v0.1.0): the compiled base was ".../keysynth/api" and requests appended "/api/...", producing
// "/keysynth/api/api/devices/me" (404 behind nginx, which maps /keysynth/api/ -> app /api/). The base is the
// service root; every request path starts with "/api/".
TEST_CASE("Cloud: default API base yields single /api prefix", "[cloud]") {
    const std::string base = ks::cloud::kDefaultApiBase;
    REQUIRE(base == "https://mvv42.ru/keysynth");
    REQUIRE((base + "/api/devices/me") == "https://mvv42.ru/keysynth/api/devices/me");
    REQUIRE(base.find("/api") == std::string::npos);
}
