// Sample cache + downloader against the fake API: jobs, shared files, resume (Range/If-Range), checksum, bad paths,
// refcounted delete, cancel, cache.json cleanup, and sampler auto-fetch on patch load (PROTOCOL.md).

#include "FakeCloud.h"

#include "cloud/CloudClient.h"
#include "cloud/CloudServices.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "core/RackGraph.h"
#include "core/Sha256.h"
#include "instruments/sampler/SamplePaths.h"
#include "preset/PatchJson.h"
#include "samples/SampleService.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#if defined(KS_HAS_SFIZZ)
#include "instruments/sampler/SamplerModule.h"
#endif

using namespace ks;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string bytesOf(size_t n, int seed) {
    std::string s(n, '\0');
    std::mt19937 rng(static_cast<unsigned>(seed));
    for (auto& c : s) c = static_cast<char>(rng());
    return s;
}

// 16-bit mono PCM WAV, 0.2 s sine.
std::string wavBytes() {
    const int sr = 48000, n = sr / 5;
    std::string d;
    auto u32 = [&](uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { d.append(reinterpret_cast<const char*>(&v), 2); };
    d += "RIFF";
    u32(36 + n * 2);
    d += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(sr);
    u32(sr * 2);
    u16(2);
    u16(16);
    d += "data";
    u32(n * 2);
    for (int i = 0; i < n; ++i) u16(static_cast<uint16_t>(static_cast<int16_t>(12000 * std::sin(i * 0.06))));
    return d;
}

struct Lib {
    std::string id;
    std::vector<std::pair<std::string, std::string>> files; // path, bytes
    std::vector<std::pair<std::string, std::vector<size_t>>> entries;
};

std::string manifestOf(const std::vector<Lib>& libs, const std::map<std::string, std::string>& shaOverride = {}) {
    json ls = json::array();
    for (const auto& l : libs) {
        json files = json::array(), entries = json::array();
        for (const auto& [p, b] : l.files) {
            auto it = shaOverride.find(p);
            files.push_back({{"path", p}, {"bytes", b.size()}, {"sha256", it != shaOverride.end() ? it->second : Sha256::hex(b)}});
        }
        for (const auto& [sfz, idx] : l.entries)
            entries.push_back({{"id", l.id + "/" + sfz}, {"sfz", sfz}, {"name", sfz}, {"files", idx}});
        ls.push_back({{"id", l.id}, {"name", l.id}, {"group", "g"}, {"licence", "CC0"}, {"author", "a"},
                      {"attribution", "x"}, {"files", files}, {"entries", entries}});
    }
    return json{{"format", 1}, {"groups", {{{"id", "g"}, {"name", "Group"}}}}, {"libraries", ls}}.dump();
}

struct CacheFixture {
    fs::path root;
    std::shared_ptr<test::FakeCloud> fake = std::make_shared<test::FakeCloud>();
    std::unique_ptr<cloud::CloudClient> client;
    std::unique_ptr<samples::SampleService> svc;
    std::vector<json> events;

    explicit CacheFixture(const std::vector<Lib>& libs, const std::map<std::string, std::string>& shaOverride = {}) {
        root = fs::temp_directory_path() / ("ks-cache-" + std::to_string(std::random_device{}()));
        fs::remove_all(root);
        fs::create_directories(root);
        for (const auto& l : libs)
            for (const auto& [p, b] : l.files) fake->files[l.id + "/" + p] = b;
        fake->manifest = manifestOf(libs, shaOverride);
        client = std::make_unique<cloud::CloudClient>(fake, test::kFakeBase, root / "cloud.json");
        client->load();
        std::string err;
        REQUIRE(client->storeLink(test::kFakeToken, {"17", "Max", ""}, err));
        makeService();
    }
    void makeService() {
        samples::SampleService::Options o;
        o.cacheRoot = root / "samples";
        o.localAssetDirs = {root / "assets"};
        o.retryBaseMs = 1;
        svc = std::make_unique<samples::SampleService>(*client, o);
        svc->start();
        REQUIRE(svc->refreshManifest(1000) == "ok");
    }
    ~CacheFixture() {
        svc.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void waitIdle() {
        for (int i = 0; i < 1000; ++i) {
            drain();
            if (svc->idle()) {
                drain();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("sample jobs did not finish");
    }
    void drain() {
        bool ended = false;
        for (auto& e : svc->poll(ended)) events.push_back(std::move(e));
    }
    const json* event(const std::string& type) {
        for (const auto& e : events)
            if (e["type"] == type) return &e;
        return nullptr;
    }
    fs::path cached(const std::string& lib, const std::string& p) { return root / "samples" / lib / pathFromUtf8(p); }
    json entryStatus(const std::string& id) {
        const json l = svc->listJson("ok");
        for (const auto& g : l["groups"])
            for (const auto& lib : g["libraries"])
                for (const auto& e : lib["entries"])
                    if (e["id"] == id) return e;
        return nullptr;
    }
};

const std::string kSpace = "Grand Piano/A0 v1.flac";
const std::string kCyr = "Рояль/Нота #1.wav";

Lib lib1() {
    return {"lib1",
            {{"one.sfz", "<region> sample=" + kSpace}, {kSpace, bytesOf(70000, 1)}, {kCyr, bytesOf(5000, 2)}, {"two.sfz", "x"}},
            {{"one.sfz", {0, 1}}, {"two.sfz", {3, 1, 2}}}};
}

} // namespace

TEST_CASE("sample cache: fetch entry, shared files, status, cache.json", "[cloud][samplecache]") {
    CacheFixture f({lib1()});
    CHECK(f.entryStatus("lib1/one.sfz")["status"] == "missing");
    auto r = f.svc->fetch("entry", "lib1/one.sfz");
    REQUIRE(r.error.empty());
    CHECK(r.job == "entry:lib1/one.sfz");
    CHECK(f.svc->fetch("entry", "lib1/one.sfz").existing);
    f.waitIdle();
    REQUIRE(f.event("samples_done"));
    CHECK((*f.event("samples_done"))["entries"] == json::array({"lib1/one.sfz"}));
    CHECK(f.event("samples_progress"));
    CHECK(fs::file_size(f.cached("lib1", kSpace)) == 70000);
    CHECK(f.entryStatus("lib1/one.sfz")["status"] == "cached");
    CHECK(f.entryStatus("lib1/two.sfz")["status"] == "partial"); // shares the flac
    // The file was requested percent-encoded (space, Cyrillic, '#').
    bool encoded = false;
    for (const auto& q : f.fake->requests) encoded = encoded || q.find("Grand%20Piano/A0%20v1.flac") != std::string::npos;
    CHECK(encoded);

    // library fetch only downloads what is missing
    const int before = f.fake->fileRequests.load();
    REQUIRE(f.svc->fetch("library", "lib1").error.empty());
    f.waitIdle();
    CHECK(f.fake->fileRequests.load() - before == 2);
    CHECK(fs::exists(f.cached("lib1", kCyr)));
    CHECK(f.entryStatus("lib1/two.sfz")["status"] == "cached");

    // cache.json survives a restart; a vanished file is dropped from it
    fs::remove(f.cached("lib1", kCyr));
    f.svc.reset();
    f.makeService();
    CHECK(f.entryStatus("lib1/one.sfz")["status"] == "cached");
    CHECK(f.entryStatus("lib1/two.sfz")["status"] == "partial");

    CHECK(f.svc->fetch("entry", "lib1/nope.sfz").error == "not_found");
    CHECK(f.svc->fetch("weird", "x").error == "bad_request");
}

TEST_CASE("sample cache: resume with Range + If-Range after a dropped transfer", "[cloud][samplecache]") {
    CacheFixture f({lib1()});
    f.fake->dropAfterBytes = 30000;
    REQUIRE(f.svc->fetch("entry", "lib1/one.sfz").error.empty());
    f.waitIdle();
    REQUIRE(f.event("samples_done"));
    bool ranged = false;
    for (const auto& q : f.fake->requests) ranged = ranged || q.find(" bytes=30000-") != std::string::npos;
    CHECK(ranged);
    std::ifstream in(f.cached("lib1", kSpace), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str() == f.fake->files["lib1/" + kSpace]);
    CHECK_FALSE(fs::exists(fs::path(f.cached("lib1", kSpace)) += ".part"));
}

TEST_CASE("sample cache: file changed on the server restarts from 0", "[cloud][samplecache]") {
    CacheFixture f({lib1()});
    // A stale .part with an unknown etag: the server answers 200 (If-Range mismatch) -> rewritten from 0.
    const fs::path part = fs::path(f.cached("lib1", kSpace)) += ".part";
    fs::create_directories(part.parent_path());
    std::ofstream(part, std::ios::binary) << std::string(1000, 'z');
    REQUIRE(f.svc->fetch("entry", "lib1/one.sfz").error.empty());
    f.waitIdle();
    REQUIRE(f.event("samples_done"));
    CHECK(Sha256::hexOfFile(f.cached("lib1", kSpace)) == Sha256::hex(f.fake->files["lib1/" + kSpace]));
}

TEST_CASE("sample cache: checksum mismatch, 404, retries, bad paths fail the job", "[cloud][samplecache]") {
    SECTION("checksum") {
        CacheFixture f({lib1()}, {{kCyr, std::string(64, 'a')}});
        REQUIRE(f.svc->fetch("entry", "lib1/two.sfz").error.empty());
        f.waitIdle();
        REQUIRE(f.event("samples_error"));
        CHECK((*f.event("samples_error"))["code"] == "checksum_mismatch");
        CHECK((*f.event("samples_error"))["notify"] == true);
        CHECK_FALSE(fs::exists(f.cached("lib1", kCyr)));
    }
    SECTION("transient 500s are retried") {
        CacheFixture f({lib1()});
        f.fake->fileFailures = 2;
        REQUIRE(f.svc->fetch("entry", "lib1/one.sfz").error.empty());
        f.waitIdle();
        CHECK(f.event("samples_done"));
        CHECK_FALSE(f.event("samples_error"));
    }
    SECTION("404") {
        CacheFixture f({lib1()});
        f.fake->files.erase("lib1/" + kCyr);
        REQUIRE(f.svc->fetch("entry", "lib1/two.sfz").error.empty());
        f.waitIdle();
        REQUIRE(f.event("samples_error"));
        CHECK((*f.event("samples_error"))["code"] == "not_found");
        CHECK((*f.event("samples_error"))["path"] == "lib1/" + kCyr);
    }
    SECTION("hostile manifest paths never touch the disk") {
        Lib evil{"evil", {{"a.sfz", "x"}, {"../../escape.wav", "boom"}, {"C:/x.wav", "boom"}, {"CON.wav", "boom"}},
                 {{"a.sfz", {0, 1, 2, 3}}}};
        CacheFixture f({evil});
        REQUIRE(f.svc->fetch("entry", "evil/a.sfz").error.empty());
        f.waitIdle();
        REQUIRE(f.event("samples_error"));
        CHECK((*f.event("samples_error"))["code"] == "bad_path");
        CHECK_FALSE(fs::exists(f.root / "escape.wav"));
        CHECK_FALSE(fs::exists(f.root / "samples" / "escape.wav"));
    }
}

TEST_CASE("sample cache: refcounted delete, in_use, read_only, cancel", "[cloud][samplecache]") {
    Lib l2{"local", {{"l.sfz", "x"}}, {{"l.sfz", {0}}}};
    CacheFixture f({lib1(), l2});
    fs::create_directories(f.root / "assets" / "samples" / "local");
    std::ofstream(f.root / "assets" / "samples" / "local" / "l.sfz") << "x";
    CHECK(f.entryStatus("local/l.sfz")["status"] == "local");
    CHECK(f.svc->remove("entry", "local/l.sfz", {}).error == "read_only");

    REQUIRE(f.svc->fetch("library", "lib1").error.empty());
    f.waitIdle();
    CHECK(f.svc->remove("entry", "lib1/one.sfz", {"lib1/one.sfz"}).error == "in_use");
    auto r = f.svc->remove("entry", "lib1/one.sfz", {});
    REQUIRE(r.error.empty());
    CHECK(r.freedBytes > 0);
    CHECK_FALSE(fs::exists(f.cached("lib1", "one.sfz")));
    CHECK(fs::exists(f.cached("lib1", kSpace))); // still used by two.sfz
    CHECK(f.entryStatus("lib1/two.sfz")["status"] == "cached");
    r = f.svc->remove("library", "lib1", {});
    REQUIRE(r.error.empty());
    CHECK_FALSE(fs::exists(f.root / "samples" / "lib1"));
    CHECK(f.entryStatus("lib1/two.sfz")["status"] == "missing");

    // Cancel: the job ends with samples_done { cancelled: true }
    f.events.clear();
    f.fake->dropAfterBytes = 0;
    REQUIRE(f.svc->fetch("all", "").error.empty());
    const auto c = f.svc->cancel("");
    CHECK(c.jobs == std::vector<std::string>{"all"});
    f.waitIdle();
    const json* done = f.event("samples_done");
    REQUIRE(done);
    CHECK((*done)["cancelled"] == true);
}

#if defined(KS_HAS_SFIZZ)
TEST_CASE("sample cache: sampler auto-fetches a missing SFZ on patch load", "[cloud][samplecache][sampler]") {
    const fs::path root = fs::temp_directory_path() / ("ks-autofetch-" + std::to_string(std::random_device{}()));
    fs::remove_all(root);
    fs::create_directories(root / "presets" / "factory");
    auto fake = std::make_shared<test::FakeCloud>();
    Lib lib{"autolib", {{"inst.sfz", "<region> sample=tone.wav lokey=0 hikey=127 pitch_keycenter=60\n"}, {"tone.wav", wavBytes()}},
            {{"inst.sfz", {0, 1}}}};
    for (const auto& [p, b] : lib.files) fake->files["autolib/" + p] = b;
    fake->manifest = manifestOf({lib});
    {
        Engine engine;
        engine.prepare(48000.0, 64);
        Session session(engine, defaultRegistry(), AppPaths::fromRoot(root));
        session.rebuild(false);
        std::vector<std::string> logs;
        session.log = [&](const std::string& level, const std::string& msg, bool) { logs.push_back(level + ": " + msg); };
        cloud::CloudServices::Options o;
        o.baseUrl = test::kFakeBase;
        o.http = fake;
        o.retryBaseMs = 1;
        o.syncIntervalMs = 3600000;
        cloud::CloudServices svc(session, o);
        svc.start();
        std::string err;
        REQUIRE(svc.client().storeLink(test::kFakeToken, {"17", "Max", ""}, err));
        REQUIRE(svc.samples().refreshManifest(1000) == "ok");

        json patch = {{"format", 2},
                      {"layers", {{{"instrument", {{"type", "sampler"}, {"state", {{"sfz", "assets/samples/autolib/inst.sfz"}}}}}}}}};
        std::vector<std::string> w;
        session.setPatch(patchFromJson(patch, &w), "", false);
        auto* sampler = [&]() -> SamplerModule* {
            for (ModuleNode* n : engine.latestGraph()->allModuleNodes())
                if (auto* s = dynamic_cast<SamplerModule*>(n->module.get())) return s;
            return nullptr;
        }();
        REQUIRE(sampler);
        CHECK(sampler->waitingForDownload());
        CHECK(sampler->loadError().empty());
        CHECK(sampler->params().get(SamplerModule::Loading) == 1.0f);
        bool done = false;
        for (int i = 0; i < 1000 && !(done && sampler->regionCount() > 0); ++i) {
            for (auto& a : svc.poll()) done = done || a.out.message["type"] == "samples_done";
            session.pollModuleErrors();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(done);
        CHECK(sampler->regionCount() > 0);
        CHECK(logs.empty()); // no missing-sfz error
        svc.stop();
        setSampleCacheDir(AppPaths::discover().sampleCache);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif
