// Preset sync algorithm (cloud-api spec §3.4) against the fake API.

#include "FakeCloud.h"

#include "cloud/SyncService.h"
#include "core/AppPaths.h"
#include "core/Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace ks;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct SyncFixture {
    fs::path dir, syncFile;
    std::shared_ptr<test::FakeCloud> fake = std::make_shared<test::FakeCloud>();
    std::mutex fileMutex;
    std::function<void(const cloud::HttpRequest&)> beforeSend; // race injection

    SyncFixture() {
        const fs::path root = fs::temp_directory_path() / ("ks-sync-" + std::to_string(std::random_device{}()));
        fs::remove_all(root);
        dir = root / "presets";
        syncFile = root / "sync.json";
        fs::create_directories(dir);
    }
    ~SyncFixture() {
        std::error_code ec;
        fs::remove_all(dir.parent_path(), ec);
    }
    cloud::SyncEngine engine() {
        return cloud::SyncEngine(
            [this](cloud::HttpRequest r) {
                if (beforeSend) beforeSend(r);
                r.url = test::kFakeBase + r.url;
                r.headers.emplace_back("Authorization", "Bearer " + test::kFakeToken);
                return fake->send(r, nullptr, nullptr);
            },
            dir, syncFile, &fileMutex);
    }
    cloud::SyncPassResult pass() { return engine().runPass(); }
    void writeLocal(const std::string& slug, const json& j) { std::ofstream(dir / (slug + ".json"), std::ios::binary) << j.dump(2) << "\n"; }
    json readLocal(const std::string& slug) {
        std::ifstream f(dir / (slug + ".json"));
        return json::parse(f, nullptr, false);
    }
    bool hasLocal(const std::string& slug) { return fs::exists(dir / (slug + ".json")); }
    void server(const std::string& slug, int64_t rev, const json& patch, bool deleted = false) {
        fake->presets[slug] = {rev, deleted, deleted ? json(nullptr) : patch};
    }
    size_t conflictCount() { return cloud::SyncEngine::conflicts(dir).size(); }
};

json patch(const std::string& name) { return {{"format", 2}, {"meta", {{"name", name}}}}; }

} // namespace

TEST_CASE("sync: initial link merges both sides", "[cloud][sync]") {
    SyncFixture f;
    f.writeLocal("local-only", patch("L"));
    f.writeLocal("same", patch("S"));
    f.writeLocal("differs", patch("mine"));
    f.server("server-only", 3, patch("R"));
    f.server("same", 2, patch("S"));
    f.server("differs", 5, patch("theirs"));
    f.server("gone", 4, nullptr, true);
    const auto r = f.pass();
    REQUIRE(r.ok);
    CHECK(f.fake->presets["local-only"].revision == 1);
    CHECK(f.readLocal("server-only")["meta"]["name"] == "R");
    CHECK(f.readLocal("same")["meta"]["name"] == "S");
    CHECK(f.readLocal("differs")["meta"]["name"] == "theirs"); // server wins
    REQUIRE(f.conflictCount() == 1);
    const auto c = cloud::SyncEngine::conflicts(f.dir)[0];
    CHECK(c.slug == "differs");
    CHECK(c.file.rfind("userdata/presets/differs.conflict-", 0) == 0);
    CHECK_FALSE(f.hasLocal("gone"));
    CHECK(f.engine().pendingCount() == 0);
    // Second pass: nothing to do, conflict files never uploaded.
    const size_t before = f.fake->requests.size();
    REQUIRE(f.pass().ok);
    CHECK(f.fake->requests.size() == before + 1); // only GET /api/presets
    CHECK(f.fake->presets.size() == 5);
}

TEST_CASE("sync: push edits, pull edits, conflicts, tombstones", "[cloud][sync]") {
    SyncFixture f;
    f.writeLocal("a", patch("a1"));
    f.writeLocal("b", patch("b1"));
    f.writeLocal("c", patch("c1"));
    f.writeLocal("d", patch("d1"));
    REQUIRE(f.pass().ok);
    for (const char* s : {"a", "b", "c", "d"}) REQUIRE(f.fake->presets[s].revision == 1);

    SECTION("local edit -> PUT If-Match") {
        f.writeLocal("a", patch("a2"));
        CHECK(f.engine().pendingCount() == 1);
        REQUIRE(f.pass().ok);
        CHECK(f.fake->presets["a"].revision == 2);
        CHECK(f.fake->presets["a"].patch["meta"]["name"] == "a2");
        CHECK(f.engine().pendingCount() == 0);
    }
    SECTION("server edit -> download") {
        f.server("b", 2, patch("b-server"));
        REQUIRE(f.pass().ok);
        CHECK(f.readLocal("b")["meta"]["name"] == "b-server");
    }
    SECTION("both edited -> server wins, local kept as conflict") {
        f.server("c", 2, patch("c-server"));
        f.writeLocal("c", patch("c-local"));
        REQUIRE(f.pass().ok);
        CHECK(f.readLocal("c")["meta"]["name"] == "c-server");
        REQUIRE(f.conflictCount() == 1);
        std::ifstream cf(f.dir / fs::path(cloud::SyncEngine::conflicts(f.dir)[0].file).filename());
        CHECK(json::parse(cf)["meta"]["name"] == "c-local");
        CHECK(f.fake->presets["c"].revision == 2);
    }
    SECTION("server tombstone vs local edit -> edit wins") {
        f.server("d", 2, nullptr, true);
        f.writeLocal("d", patch("d-revived"));
        REQUIRE(f.pass().ok);
        CHECK(f.fake->presets["d"].revision == 3);
        CHECK_FALSE(f.fake->presets["d"].deleted);
        CHECK(f.fake->presets["d"].patch["meta"]["name"] == "d-revived");
    }
    SECTION("server tombstone, local unchanged -> local deleted") {
        f.server("d", 2, nullptr, true);
        REQUIRE(f.pass().ok);
        CHECK_FALSE(f.hasLocal("d"));
        REQUIRE(f.pass().ok); // stays deleted, no resurrection
        CHECK_FALSE(f.hasLocal("d"));
        CHECK(f.fake->presets["d"].deleted);
    }
    SECTION("local delete -> DELETE If-Match (tombstone)") {
        fs::remove(f.dir / "a.json");
        CHECK(f.engine().pendingCount() == 1);
        REQUIRE(f.pass().ok);
        CHECK(f.fake->presets["a"].deleted);
        CHECK(f.fake->presets["a"].revision == 2);
        REQUIRE(f.pass().ok);
        CHECK_FALSE(f.hasLocal("a"));
    }
    SECTION("local delete vs server edit -> server edit wins") {
        fs::remove(f.dir / "a.json");
        f.server("a", 2, patch("a-server"));
        REQUIRE(f.pass().ok);
        CHECK(f.readLocal("a")["meta"]["name"] == "a-server");
        CHECK_FALSE(f.fake->presets["a"].deleted);
    }
    SECTION("412 race on push -> conflict copy, server version") {
        f.writeLocal("a", patch("a-local"));
        f.beforeSend = [&](const cloud::HttpRequest& r) {
            if (r.method == "PUT") {
                f.server("a", 2, patch("a-raced"));
                f.beforeSend = nullptr;
            }
        };
        REQUIRE(f.pass().ok);
        CHECK(f.readLocal("a")["meta"]["name"] == "a-raced");
        CHECK(f.conflictCount() == 1);
    }
    SECTION("server row purged -> re-upload") {
        f.fake->presets.erase("b");
        REQUIRE(f.pass().ok);
        CHECK(f.fake->presets.count("b"));
        CHECK(f.fake->presets["b"].revision == 1);
    }
    SECTION("offline -> error, nothing changes") {
        f.writeLocal("a", patch("a3"));
        f.fake->offline = true;
        const auto r = f.pass();
        CHECK_FALSE(r.ok);
        CHECK(r.error == "offline");
        CHECK(f.engine().pendingCount() == 1);
    }
}

TEST_CASE("sync: hostile server slugs and invalid local files are ignored", "[cloud][sync]") {
    SyncFixture f;
    f.server("../../evil", 1, patch("x"));
    f.server("Upper", 1, patch("x"));
    f.server("ok-one", 1, patch("ok"));
    std::ofstream(f.dir / "not a slug.json") << "{}";
    f.writeLocal("broken", json::array()); // not a patch object: not uploaded
    const auto r = f.pass();
    CHECK(f.hasLocal("ok-one"));
    CHECK_FALSE(fs::exists(f.dir.parent_path().parent_path() / "evil.json"));
    CHECK_FALSE(f.hasLocal("Upper"));
    CHECK_FALSE(f.fake->presets.count("not a slug"));
    CHECK_FALSE(f.fake->presets.count("broken"));
    CHECK(r.error == "invalid_patch");
    CHECK(cloud::isValidSlug("a-b-1"));
    for (const char* bad : {"", "-a", "a-", "a--b", "A", "a.b", "a/b", "a.conflict-20261012T140300Z", "con", "nul", "com1", "lpt9"})
        CHECK_FALSE(cloud::isValidSlug(bad));
}
