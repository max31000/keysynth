// Relay host (ARCHITECTURE §15 *Relay*): remote trust table (derived from the PROTOCOL.md message list), envelope
// routing through ControlServer's virtual clients, RemoteRouter rates (telemetry / midi / param) and pause,
// RelayClient connect / backoff / heartbeat / 401 with a fake socket, and the WinHTTP socket against a local server.

#include "FakeCloud.h"

#include "cloud/CloudClient.h"
#include "cloud/RelayClient.h"
#include "control/ControlServer.h"
#include "control/ProtocolHandler.h"
#include "control/RemoteRouter.h"
#include "control/Session.h"
#include "control/Trust.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/ModuleRegistry.h"
#include "platform/WinHttpWebSocket.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

using namespace ks;
using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path tempDir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path() / ("ks-relay-" + tag + "-" + std::to_string(std::random_device{}()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// Every message type of PROTOCOL.md's "Client → engine" table.
std::set<std::string> protocolRequestTypes() {
    std::ifstream f(fs::path(KS_SOURCE_DIR) / "docs" / "PROTOCOL.md", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string doc = ss.str();
    const size_t from = doc.find("## Client \xE2\x86\x92 engine");
    const size_t to = doc.find("\n## ", from + 4);
    std::set<std::string> types;
    if (from == std::string::npos) return types;
    std::istringstream lines(doc.substr(from, to - from));
    std::string line;
    static const std::regex tick("`([a-z_]+)`");
    while (std::getline(lines, line)) {
        if (line.rfind("| `", 0) != 0) continue;
        const std::string cell = line.substr(1, line.find('|', 1) - 1);
        for (std::sregex_iterator it(cell.begin(), cell.end(), tick), end; it != end; ++it) types.insert((*it)[1].str());
    }
    return types;
}

} // namespace

TEST_CASE("relay trust: remote allow/deny for every PROTOCOL request", "[relay][protocol]") {
    // A representative valid request per type and the expected verdict for `remote` sessions (spec §10.3). A new
    // message in PROTOCOL.md fails this test until it is classified here (default-deny in the engine).
    const std::map<std::string, std::pair<json, bool>> table = {
        {"hello", {{}, true}},
        {"get_catalog", {{}, true}},
        {"set_param", {{{"node", 0}, {"param", "volume_db"}, {"value", -6}}, true}},
        {"load_preset", {{{"path", "presets/factory/piano.json"}}, true}},
        {"save_preset", {{{"name", "x"}}, true}},
        {"list_presets", {{}, true}},
        {"get_patch", {{}, true}},
        {"set_patch", {{{"patch", {{"layers", json::array()}}}}, true}},
        {"add_layer", {{}, true}},
        {"remove_layer", {{{"layer", 1}}, true}},
        {"set_zone", {{{"layer", 1}, {"zone", json::object()}}, true}},
        {"set_instrument", {{{"layer", 1}, {"module", "va"}}, true}},
        {"add_fx", {{{"layer", 0}, {"module", "reverb"}}, true}},
        {"remove_fx", {{{"node", 5}}, true}},
        {"move_fx", {{{"node", 5}, {"to", 0}}, true}},
        {"set_fx_bypass", {{{"node", 5}, {"bypass", true}}, true}},
        {"rescan_midi", {{}, true}},
        {"note", {{{"on", true}, {"note", 60}, {"velocity", 100}}, true}},
        {"cc", {{{"cc", 64}, {"value", 127}}, true}},
        {"transport", {{{"playing", true}, {"pattern", "presets/patterns/basic-rock.json"}}, true}},
        {"list_patterns", {{}, true}},
        {"get_pattern", {{{"path", "userdata/patterns/x.json"}}, true}},
        {"set_pattern", {{{"pattern", json::object()}}, true}},
        {"save_pattern", {{{"name", "x"}}, true}},
        {"list_devices", {{}, true}},
        {"set_audio_device", {{{"device_type", "ASIO"}, {"name", "x"}}, false}},
        {"set_audio_input", {{{"channels", json::array({0})}}, false}},
        {"open_audio_panel", {{}, false}},
        {"restart_audio", {{}, true}},
        {"panic", {{}, true}},
        {"list_plugins", {{}, true}},
        {"reload_plugin", {{{"name", "x"}}, false}},
        {"cloud_link", {{{"token", test::kFakeToken}}, false}},
        {"cloud_unlink", {{}, false}},
        {"sync_now", {{}, true}},
        {"samples_list", {{}, true}},
        {"samples_fetch", {{{"scope", "all"}}, true}},
        {"samples_cancel", {{}, true}},
        {"samples_delete", {{{"scope", "all"}}, false}},
        {"chord_learn", {{{"node", 5}}, true}},
        {"song_list", {{}, true}},
        {"song_get", {{{"path", "presets/songs/x.json"}}, true}},
        {"song_save", {{{"song", json::object()}}, true}},
        {"song_delete", {{{"path", "userdata/songs/x.json"}}, true}},
        {"song_select", {{{"path", "userdata/songs/x.json"}}, true}},
        {"song_section", {{{"index", 1}}, true}},
        {"song_next", {{}, true}},
        {"song_prev", {{}, true}},
        {"song_close", {{}, true}},
        {"song_control_keys", {{{"enabled", true}}, true}},
    };
    const std::set<std::string> documented = protocolRequestTypes();
    REQUIRE(documented.size() >= 30);
    for (const auto& t : documented) {
        INFO("PROTOCOL.md request '" << t << "' needs a remote-trust verdict in this table");
        CHECK(table.count(t) == 1);
    }
    for (const auto& [type, entry] : table) {
        INFO(type);
        CHECK(documented.count(type) == 1); // the table only lists documented messages
        json m = entry.first.is_null() ? json::object() : entry.first;
        m["type"] = type;
        CHECK(remoteRestriction(m).empty() == entry.second);
        CHECK(trustRestriction(Trust::Remote, m).empty() == entry.second);
        CHECK(trustRestriction(Trust::Local, m).empty());
    }
    // Paths: logical ids only (web rules).
    CHECK_FALSE(remoteRestriction({{"type", "load_preset"}, {"path", "C:/Windows/win.ini"}}).empty());
    CHECK_FALSE(remoteRestriction({{"type", "load_preset"}, {"path", "presets/factory/../../x.json"}}).empty());
    CHECK_FALSE(remoteRestriction({{"type", "transport"}, {"pattern", "D:/p.json"}}).empty());
    CHECK_FALSE(
        remoteRestriction({{"type", "set_patch"},
                           {"patch", {{"layers", {{{"instrument", {{"state", {{"sfz", "C:/x.sfz"}}}}}}}}}}})
            .empty());
    CHECK_FALSE(remoteRestriction({{"type", "song_select"}, {"path", "C:/songs/x.json"}}).empty());
    CHECK_FALSE(remoteRestriction({{"type", "song_get"}, {"path", "presets/factory/x.json"}}).empty());
    CHECK_FALSE(remoteRestriction({{"type", "song_delete"}}).empty());
    CHECK_FALSE(
        webRestriction({{"type", "set_patch"},
                        {"patch", {{"layers", {{{"midi", {{{"type", "arp"}, {"state", {{"path", "C:/x"}}}}}}}}}}}})
            .empty());
    // Unknown / future messages.
    CHECK_FALSE(remoteRestriction({{"type", "song_load"}}).empty());
    CHECK_FALSE(remoteRestriction({{"type", "pause"}}).empty());
    CHECK(std::string(toString(Trust::Remote)) == "remote");
}

namespace {

struct ServerFixture {
    fs::path root;
    Engine engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<ProtocolHandler> handler;
    std::unique_ptr<ControlServer> server;
    std::vector<json> sent; // envelopes
    std::vector<bool> lossy;

    ServerFixture() {
        root = tempDir("srv");
        fs::create_directories(root / "presets");
        fs::copy(fs::path(KS_SOURCE_DIR) / "presets" / "factory", root / "presets" / "factory",
                 fs::copy_options::recursive);
        fs::copy(fs::path(KS_SOURCE_DIR) / "presets" / "patterns", root / "presets" / "patterns",
                 fs::copy_options::recursive);
        engine.prepare(48000.0, 64);
        session = std::make_unique<Session>(engine, defaultRegistry(), AppPaths::fromRoot(root));
        session->rebuild(false);
        handler = std::make_unique<ProtocolHandler>(*session);
        server = std::make_unique<ControlServer>(*handler, [](std::function<void()> f) { f(); }, root / "no-ui");
        server->setRemoteSink([this](const json& e, bool l) {
            sent.push_back(e);
            lossy.push_back(l);
        });
    }
    ~ServerFixture() {
        server.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::vector<json> take() {
        std::vector<json> s;
        s.swap(sent);
        lossy.clear();
        return s;
    }
};

std::string firstFactoryPreset(const fs::path& root) {
    for (const auto& e : fs::recursive_directory_iterator(root / "presets" / "factory"))
        if (e.path().extension() == ".json")
            return "presets/factory/" + fs::relative(e.path(), root / "presets" / "factory").generic_string();
    return {};
}

} // namespace

TEST_CASE("relay routing: virtual clients, replies, broadcasts, except, deny", "[relay][protocol]") {
    ServerFixture f;
    f.server->remoteJoined("a");
    f.server->remoteJoined("b");
    CHECK(f.server->clientCount() == 2);
    CHECK(f.server->remoteCount() == 2);

    // hello -> reply to the sender only, trust "remote".
    f.server->remoteMessage("a", {{"type", "hello"}, {"id", 1}});
    auto out = f.take();
    REQUIRE(out.size() >= 1);
    CHECK(out[0]["t"] == "msg");
    CHECK(out[0]["to"] == "a");
    CHECK_FALSE(out[0].contains("except"));
    CHECK_FALSE(out[0].contains("lossy"));
    CHECK(out[0]["msg"]["type"] == "state");
    CHECK(out[0]["msg"]["id"] == 1);
    CHECK(out[0]["msg"]["trust"] == "remote");
    for (const auto& e : out) CHECK(e["to"] == "a"); // + sync_status, still to the sender

    // set_param -> param echo to the others: "*" except the sender.
    f.server->remoteMessage("a", {{"type", "set_param"}, {"node", 0}, {"param", "volume_db"}, {"value", -7.0}});
    out = f.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0]["to"] == "*");
    CHECK(out[0]["except"] == "a");
    CHECK(out[0]["msg"]["type"] == "param");
    CHECK(out[0]["msg"]["value"] == -7.0);

    // load_preset (logical path) -> reply + state broadcast to all.
    const std::string preset = firstFactoryPreset(f.root);
    REQUIRE_FALSE(preset.empty());
    f.server->remoteMessage("b", {{"type", "load_preset"}, {"id", 7}, {"path", preset}});
    out = f.take();
    REQUIRE(out.size() >= 2);
    CHECK(out[0]["to"] == "b");
    CHECK(out[0]["msg"]["type"] == "load_preset_ok");
    bool stateToAll = false;
    for (const auto& e : out)
        if (e["msg"]["type"] == "state" && e["to"] == "*" && !e.contains("except")) stateToAll = true;
    CHECK(stateToAll);

    // Denied for remote sessions.
    for (const char* t : {"cloud_unlink", "set_audio_device", "open_audio_panel", "reload_plugin", "samples_delete"}) {
        f.server->remoteMessage("a", {{"type", t}, {"id", 9}});
        out = f.take();
        REQUIRE(out.size() == 1);
        CHECK(out[0]["to"] == "a");
        CHECK(out[0]["msg"]["type"] == "error");
        CHECK(out[0]["msg"]["code"] == "forbidden");
        CHECK(out[0]["msg"]["id"] == 9);
    }

    // Engine-side broadcasts (log, plugin_status, ...) reach remote controllers once, as "*".
    f.server->broadcast({{"type", "log"}, {"level", "info"}, {"message", "x"}});
    out = f.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0]["to"] == "*");
    // A local client's Others: every remote controller.
    f.server->sendOthers("local-1", {{"type", "param"}, {"node", 0}, {"param", "x"}, {"value", 1}});
    out = f.take();
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].contains("except"));
    // Async replies (cloud) addressed to a remote client.
    f.server->dispatchAsync({{"r:b", {Outgoing::Target::Reply, {{"type", "samples_list_ok"}, {"id", 3}}}}});
    out = f.take();
    REQUIRE(out.size() == 1);
    CHECK(out[0]["to"] == "b");

    // Unknown cid joins implicitly; leave / clear remove the virtual clients.
    f.server->remoteMessage("c", {{"type", "list_presets"}, {"id", 2}});
    CHECK(f.server->remoteCount() == 3);
    CHECK(f.take().at(0)["to"] == "c");
    f.server->remoteLeft("c");
    CHECK(f.server->clientCount() == 2);
    f.server->sendTo("r:c", {{"type", "x"}});
    CHECK(f.take().empty()); // gone
    f.server->remoteClear();
    CHECK(f.server->clientCount() == 0);
    f.server->broadcast({{"type", "log"}});
    CHECK(f.take().empty());
}

namespace {
struct RouterFixture {
    RemoteRouter r;
    RemoteRouter::Clock::time_point t = RemoteRouter::Clock::now();
    std::vector<json> sent;
    std::vector<bool> lossy;
    RouterFixture() {
        r.setClock([this] { return t; });
        r.setSink([this](const json& e, bool l) {
            sent.push_back(e);
            lossy.push_back(l);
        });
    }
    void advance(std::chrono::milliseconds d) { t += d; }
    size_t count(const std::string& type) const {
        size_t n = 0;
        for (const auto& e : sent)
            if (e["msg"]["type"] == type) ++n;
        return n;
    }
};
} // namespace

TEST_CASE("relay router: telemetry <= 10 Hz, midi merged <= 15 Hz, lossy flag", "[relay]") {
    RouterFixture f;
    const json tele = {{"type", "telemetry"}, {"cpu", 0.1}};
    f.r.broadcast(tele);
    CHECK(f.sent.empty()); // no controllers
    f.r.join("a");
    for (int i = 0; i < 30; ++i) { // 1 s at 30 Hz
        f.r.broadcast(tele);
        f.advance(33ms);
    }
    CHECK(f.count("telemetry") >= 7); // 30 Hz pump: every 4th frame (133 ms) passes the 100 ms gate
    CHECK(f.count("telemetry") <= 10);
    REQUIRE_FALSE(f.sent.empty());
    CHECK(f.sent[0]["lossy"] == true);
    CHECK(f.lossy[0]);
    CHECK(f.sent[0]["to"] == "*");

    f.sent.clear();
    for (int i = 0; i < 30; ++i) {
        f.r.broadcast({{"type", "midi"}, {"notes", {{{"note", i}, {"on", true}, {"velocity", 90}}}}});
        f.advance(33ms);
        f.r.tick();
    }
    f.advance(100ms);
    f.r.tick();
    CHECK(f.count("midi") <= 16);
    CHECK(f.count("midi") >= 10);
    size_t notes = 0;
    for (const auto& e : f.sent) notes += e["msg"]["notes"].size();
    CHECK(notes == 30); // nothing lost, only batched
    CHECK(f.sent[0]["lossy"] == true);

    f.sent.clear();
    f.lossy.clear();
    f.r.broadcast({{"type", "samples_progress"}, {"jobs", json::array()}});
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0]["lossy"] == true);
    f.r.broadcast({{"type", "state"}});
    CHECK_FALSE(f.sent[1].contains("lossy"));
    CHECK_FALSE(f.lossy[1]);
}

TEST_CASE("relay router: param coalescing per (node,param) <= 20 Hz, latest wins", "[relay]") {
    RouterFixture f;
    f.r.join("a");
    f.r.join("b");
    auto param = [](int node, const char* p, double v) {
        return json{{"type", "param"}, {"node", node}, {"param", p}, {"value", v}};
    };
    // 60 Hz drag for 1 s on one knob + one change on another.
    for (int i = 0; i < 60; ++i) {
        f.r.broadcast(param(3, "cutoff", i), "a");
        f.advance(16ms);
        f.r.tick();
    }
    f.r.broadcast(param(4, "res", 0.5), "a");
    f.advance(60ms);
    f.r.tick();
    size_t cutoff = 0;
    json last;
    for (const auto& e : f.sent)
        if (e["msg"]["param"] == "cutoff") {
            ++cutoff;
            last = e;
        }
    CHECK(cutoff <= 21);
    CHECK(cutoff >= 15);
    CHECK(last["msg"]["value"] == 59); // the final value always arrives
    CHECK(last["except"] == "a");
    CHECK(f.count("param") == cutoff + 1); // the other key is independent (sent at once)

    // A state snapshot supersedes a coalesced value.
    f.sent.clear();
    f.r.broadcast(param(3, "cutoff", 1), "a");
    f.r.broadcast(param(3, "cutoff", 2), "a"); // pending
    f.r.broadcast({{"type", "state"}});
    f.advance(100ms);
    f.r.tick();
    CHECK(f.count("param") == 1);
    CHECK(f.sent.back()["msg"]["type"] == "state");

    // Any other non-lossy event (or a state that skips someone) flushes coalesced values first: order is kept.
    for (const json& next : {json{{"type", "log"}}, json{{"type", "state"}}}) {
        f.sent.clear();
        f.advance(100ms);
        f.r.broadcast(param(3, "cutoff", 5), "a");
        f.r.broadcast(param(3, "cutoff", 6), "a"); // pending
        f.r.broadcast(next, next["type"] == "state" ? "b" : "");
        REQUIRE(f.sent.size() == 3);
        CHECK(f.sent[1]["msg"]["value"] == 6);
        CHECK(f.sent[2]["msg"]["type"] == next["type"]);
    }

    // Others with only the sender connected: nothing to send.
    f.sent.clear();
    f.r.leave("b");
    f.advance(100ms);
    f.r.broadcast(param(9, "x", 1), "a");
    f.r.broadcast({{"type", "state"}}, "a");
    CHECK(f.sent.empty());
}

TEST_CASE("relay router: paused controllers skip telemetry/midi, nothing when all paused", "[relay]") {
    RouterFixture f;
    f.r.join("a");
    f.r.join("b");
    f.r.setPaused("b", true);
    CHECK(f.r.paused("b"));
    CHECK(f.r.activeCount() == 1);
    f.r.broadcast({{"type", "telemetry"}});
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0]["to"] == "a"); // fan-out per active cid instead of "*"
    f.r.broadcast({{"type", "log"}});
    CHECK(f.sent[1]["to"] == "*"); // state-bearing messages still reach paused tabs
    f.r.setPaused("a", true);
    f.sent.clear();
    f.advance(200ms);
    f.r.broadcast({{"type", "telemetry"}});
    f.r.broadcast({{"type", "midi"}, {"notes", {{{"note", 1}}}}});
    f.advance(200ms);
    f.r.tick();
    CHECK(f.sent.empty());
    f.r.setPaused("a", false);
    f.r.setPaused("b", false);
    f.r.broadcast({{"type", "telemetry"}});
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0]["to"] == "*");
    CHECK(f.count("midi") == 0); // notes played while paused were dropped
}

// ---------------- RelayClient with a fake socket ----------------

namespace {

struct FakeHub;

struct FakeSocket final : platform::WebSocket {
    FakeHub& hub;
    OnText onText;
    OnClose onClose;
    std::atomic<bool> open_{false};
    explicit FakeSocket(FakeHub& h) : hub(h) {}
    ~FakeSocket() override;
    platform::WsOpenResult open(const platform::WsHandshake& hs, OnText t, OnClose c) override;
    bool send(std::string text) override;
    void close(int code, const std::string& reason) override;
    void abort() override { open_ = false; } // never blocks in open(); no callback on the caller's thread
    size_t queuedBytes() const override { return 0; }
    void serverSend(const json& j) {
        if (open_) onText(j.dump());
    }
    void serverClose(int code) {
        if (open_.exchange(false)) onClose(code, "server");
    }
};

struct FakeHub {
    std::mutex m;
    std::condition_variable cv;
    std::deque<platform::WsOpenResult> script; // results of the next open() calls; empty = success
    std::vector<platform::WsHandshake> handshakes;
    std::vector<std::string> frames; // client -> server
    FakeSocket* current = nullptr;
    int opened = 0, destroyed = 0;
    std::atomic<bool> failSends{false}; // simulate a full send queue

    platform::WebSocketFactory factory() {
        return [this] { return std::make_unique<FakeSocket>(*this); };
    }
    template <class Pred>
    bool waitFor(Pred p, std::chrono::milliseconds ms = 3000ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, ms, p);
    }
    std::vector<json> framesOfType(const std::string& t) {
        std::lock_guard<std::mutex> lk(m);
        std::vector<json> out;
        for (const auto& f : frames) {
            json j = json::parse(f);
            if (j["t"] == t) out.push_back(j);
        }
        return out;
    }
    void withCurrent(const std::function<void(FakeSocket&)>& f) {
        FakeSocket* s;
        {
            std::lock_guard<std::mutex> lk(m);
            s = current;
        }
        if (s) f(*s);
    }
};

FakeSocket::~FakeSocket() {
    close(1001, "destroyed");
    std::lock_guard<std::mutex> lk(hub.m);
    if (hub.current == this) hub.current = nullptr;
    ++hub.destroyed;
    hub.cv.notify_all();
}

platform::WsOpenResult FakeSocket::open(const platform::WsHandshake& hs, OnText t, OnClose c) {
    onText = std::move(t);
    onClose = std::move(c);
    std::lock_guard<std::mutex> lk(hub.m);
    hub.handshakes.push_back(hs);
    platform::WsOpenResult r;
    if (!hub.script.empty()) {
        r = hub.script.front();
        hub.script.pop_front();
    } else {
        r.ok = true;
        r.httpStatus = 101;
    }
    if (r.ok) {
        open_ = true;
        hub.current = this;
        ++hub.opened;
    }
    hub.cv.notify_all();
    return r;
}

bool FakeSocket::send(std::string text) {
    if (!open_ || hub.failSends) return false;
    std::lock_guard<std::mutex> lk(hub.m);
    hub.frames.push_back(std::move(text));
    hub.cv.notify_all();
    return true;
}

void FakeSocket::close(int code, const std::string& reason) {
    if (open_.exchange(false)) onClose(code, reason);
}

platform::WsOpenResult failure(int status, const std::string& err) {
    platform::WsOpenResult r;
    r.httpStatus = status;
    r.error = err;
    return r;
}

// Message-thread stand-in: posted tasks queue up and run in drain().
struct PostQueue {
    std::mutex m;
    std::deque<std::function<void()>> q;
    cloud::RelayClient::Post post() {
        return [this](std::function<void()> f) {
            std::lock_guard<std::mutex> lk(m);
            q.push_back(std::move(f));
        };
    }
    void drain() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lk(m);
            run.swap(q);
        }
        for (auto& f : run) f();
    }
};

struct RelayFixture {
    fs::path dir = tempDir("client");
    std::shared_ptr<test::FakeCloud> fake = std::make_shared<test::FakeCloud>();
    cloud::CloudClient client{fake, test::kFakeBase, dir / "cloud.json"};
    FakeHub hub;
    PostQueue posts;
    std::vector<std::string> events;
    std::unique_ptr<cloud::RelayClient> relay;

    explicit RelayFixture(bool linked = true, int silenceMs = 5000) {
        client.load();
        if (linked) {
            std::string err;
            REQUIRE(client.storeLink(test::kFakeToken, {"17", "Max", ""}, err));
        }
        cloud::RelayClient::Options o;
        o.factory = hub.factory();
        o.backoffMinMs = 10;
        o.backoffMaxMs = 80;
        o.pingIntervalMs = 50;
        o.silenceTimeoutMs = silenceMs;
        o.linkPollMs = 10;
        o.stableMs = 100000;
        o.random = [] { return 0.5; };
        relay = std::make_unique<cloud::RelayClient>(client, "9.9.9", o);
        cloud::RelayClient::Handlers h;
        h.controller = [this](const std::string& cid, bool j) { events.push_back((j ? "join:" : "leave:") + cid); };
        h.message = [this](const std::string& cid, const json& m) { events.push_back("msg:" + cid + ":" + m.dump()); };
        h.paused = [this](const std::string& cid, bool p) { events.push_back((p ? "pause:" : "resume:") + cid); };
        h.cleared = [this] { events.push_back("cleared"); };
        relay->start(posts.post(), h);
    }
    ~RelayFixture() {
        relay->stop();
        relay.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("relay client: url, backoff schedule", "[relay]") {
    CHECK(cloud::RelayClient::relayUrl("https://mvv42.ru/keysynth") == "wss://mvv42.ru/keysynth/api/relay?role=host");
    CHECK(cloud::RelayClient::relayUrl("http://127.0.0.1:8766/keysynth/") ==
          "ws://127.0.0.1:8766/keysynth/api/relay?role=host");
    CHECK(platform::wsToHttpUrl("wss://a/b") == "https://a/b");
    CHECK(platform::wsToHttpUrl("ws://a/b") == "http://a/b");
    const int expect[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (int i = 0; i < 8; ++i) CHECK(cloud::RelayClient::backoffMs(i, 1000, 30000, 0.5) == expect[i]);
    CHECK(cloud::RelayClient::backoffMs(2, 1000, 30000, 0.0) == 3200); // -20 %
    CHECK(cloud::RelayClient::backoffMs(2, 1000, 30000, 1.0) == 4800); // +20 %
    CHECK(cloud::RelayClient::backoffMs(0, 1000, 30000, 0.0) == 1000); // never below the minimum
    CHECK(cloud::RelayClient::backoffMs(9, 1000, 30000, 1.0) == 30000); // never above the maximum
    CHECK(cloud::RelayClient::backoffMs(1000, 1000, 30000, 0.5) == 30000);
}

TEST_CASE("relay client: connect with bearer, hello, backoff on failures, heartbeat", "[relay]") {
    RelayFixture f(false);
    {
        std::lock_guard<std::mutex> lk(f.hub.m);
        f.hub.script = {failure(0, "offline"), failure(502, "bad gateway"), failure(0, "offline")};
    }
    std::this_thread::sleep_for(50ms);
    CHECK(f.relay->state() == cloud::RelayClient::State::Idle); // not linked: no attempt
    CHECK(f.hub.handshakes.empty());
    std::string err;
    REQUIRE(f.client.storeLink(test::kFakeToken, {"17", "Max", ""}, err));
    f.relay->wake();
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 1; }));
    CHECK(f.hub.handshakes.size() == 4);
    CHECK(f.relay->failures() == 3);     // reset only by a connection that stays up (stableMs)
    CHECK(f.relay->lastDelayMs() == 40); // 10, 20, 40
    const auto& hs = f.hub.handshakes.back();
    CHECK(hs.url == "wss://fake.test/keysynth/api/relay?role=host");
    REQUIRE(hs.headers.size() == 1);
    CHECK(hs.headers[0].first == "Authorization");
    CHECK(hs.headers[0].second == "Bearer " + test::kFakeToken);
    REQUIRE(f.hub.waitFor([&] { return !f.hub.frames.empty(); }));
    const json hello = json::parse(f.hub.frames.front());
    CHECK(hello["t"] == "hello");
    CHECK(hello["host"]["deviceId"] == f.client.deviceId());
    CHECK(hello["host"]["version"] == "9.9.9");
    CHECK(hello["host"]["name"].is_string());
    CHECK(f.relay->state() == cloud::RelayClient::State::Connected);
    // app-level ping every 50 ms (test interval); answer server pings
    REQUIRE(f.hub.waitFor([&] {
        int n = 0;
        for (const auto& s : f.hub.frames) n += s.find("\"ping\"") != std::string::npos;
        return n >= 2;
    }));
    f.hub.withCurrent([](FakeSocket& s) { s.serverSend({{"t", "ping"}}); });
    REQUIRE(f.hub.waitFor([&] {
        for (const auto& s : f.hub.frames)
            if (s.find("\"pong\"") != std::string::npos) return true;
        return false;
    }));
    // outbound envelopes
    CHECK(f.relay->send({{"t", "msg"}, {"to", "*"}, {"msg", {{"type", "state"}}}}, false));
    CHECK_FALSE(f.relay->send({{"t", "msg"}, {"to", "*"}, {"msg", {{"blob", std::string(1100000, 'x')}}}}, false));
    CHECK(f.hub.framesOfType("msg").size() == 1);
}

TEST_CASE("relay client: inbound controller events reach the message thread", "[relay]") {
    RelayFixture f;
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 1; }));
    f.hub.withCurrent([](FakeSocket& s) {
        s.serverSend({{"t", "controller"}, {"cid", "c1"}, {"joined", true}});
        s.serverSend({{"t", "msg"}, {"from", "c1"}, {"msg", {{"type", "hello"}, {"id", 1}}}});
        s.serverSend({{"t", "pause"}, {"cid", "c1"}});
        s.serverSend({{"t", "resume"}, {"cid", "c1"}});
        s.serverSend({{"t", "controller"}, {"cid", "c1"}, {"joined", false}});
        s.serverSend({{"t", "controllers"}, {"count", 0}});
        s.serverSend({{"t", "msg"}, {"msg", {{"type", "hello"}}}}); // no from: ignored
        s.serverSend({{"t", "bogus"}});
    });
    CHECK(f.events.empty()); // nothing runs off the message thread
    f.posts.drain();
    const std::vector<std::string> expect = {"join:c1", R"(msg:c1:{"id":1,"type":"hello"})", "pause:c1", "resume:c1",
                                             "leave:c1", "cleared"};
    CHECK(f.events == expect);
    // server closes -> cleared + reconnect after the backoff
    f.events.clear();
    f.hub.withCurrent([](FakeSocket& s) { s.serverClose(4008); });
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 2; }));
    f.posts.drain();
    REQUIRE_FALSE(f.events.empty());
    CHECK(f.events.front() == "cleared");
    CHECK(f.relay->connects() == 2);
}

TEST_CASE("relay client: 401/403 at the upgrade drops the token and stops", "[relay]") {
    for (int status : {401, 403}) {
        RelayFixture f(false);
        {
            std::lock_guard<std::mutex> lk(f.hub.m);
            f.hub.script = {failure(status, "refused")};
        }
        std::string err;
        REQUIRE(f.client.storeLink(test::kFakeToken, {"17", "Max", ""}, err));
        const uint64_t v = f.client.statusVersion();
        f.relay->wake();
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (f.client.linked() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(5ms);
        CHECK_FALSE(f.client.linked());
        CHECK(f.client.status().error == "token_invalid");
        CHECK(f.client.statusVersion() > v); // -> CloudServices::poll broadcasts cloud_status
        std::this_thread::sleep_for(80ms);
        CHECK(f.relay->state() == cloud::RelayClient::State::Idle);
        CHECK(f.hub.handshakes.size() == 1); // no retries without a token
        CHECK(f.hub.opened == 0);
    }
}

TEST_CASE("relay client: silence timeout and re-link reconnect", "[relay]") {
    RelayFixture f(true, 150);
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 1; }));
    // The fake server never answers: after 150 ms of silence the client reconnects.
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened >= 2; }, 3000ms));
    // A different token (re-link) closes the current socket and reconnects with it.
    std::string err;
    REQUIRE(f.client.storeLink(test::kOtherToken, {"42", "Other", ""}, err));
    REQUIRE(f.hub.waitFor([&] {
        return !f.hub.handshakes.empty() &&
               f.hub.handshakes.back().headers[0].second == "Bearer " + test::kOtherToken && f.hub.current;
    }));
    // Unlink: socket closed, idle.
    platform::WebSocket::OnText stale;
    f.hub.withCurrent([&](FakeSocket& sock) { stale = sock.onText; });
    f.client.clearLink("");
    REQUIRE(f.hub.waitFor([&] { return f.hub.current == nullptr; }));
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (f.relay->state() != cloud::RelayClient::State::Idle && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(5ms);
    CHECK(f.relay->state() == cloud::RelayClient::State::Idle);
    // A frame of the retired socket arriving late must not re-join a ghost controller after `cleared`.
    f.posts.drain();
    f.events.clear();
    REQUIRE(stale);
    stale(json{{"t", "msg"}, {"from", "ghost"}, {"msg", {{"type", "hello"}}}}.dump());
    stale(json{{"t", "controller"}, {"cid", "ghost"}, {"joined", true}}.dump());
    f.posts.drain();
    CHECK(f.events.empty());
}

TEST_CASE("relay client: non-lossy overflow resyncs, oversized reply becomes an error", "[relay]") {
    RelayFixture f;
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 1 && !f.hub.frames.empty(); }));
    // Oversized reply to a cid with an id -> error too_large to that cid.
    CHECK_FALSE(f.relay->send({{"t", "msg"},
                               {"to", "c1"},
                               {"msg", {{"type", "samples_list_ok"}, {"id", 5}, {"blob", std::string(1100000, 'x')}}}},
                              false));
    const auto msgs = f.hub.framesOfType("msg");
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0]["to"] == "c1");
    CHECK(msgs[0]["msg"]["type"] == "error");
    CHECK(msgs[0]["msg"]["code"] == "too_large");
    CHECK(msgs[0]["msg"]["id"] == 5);
    // A lossy frame that cannot be queued is just dropped ...
    f.hub.failSends = true;
    CHECK_FALSE(f.relay->send({{"t", "msg"}, {"to", "*"}, {"lossy", true}, {"msg", {{"type", "telemetry"}}}}, true));
    std::this_thread::sleep_for(30ms);
    CHECK(f.hub.opened == 1);
    // ... a non-lossy one forces a reconnect (controllers re-hello and get a fresh state).
    CHECK_FALSE(f.relay->send({{"t", "msg"}, {"to", "*"}, {"msg", {{"type", "state"}}}}, false));
    f.hub.failSends = false;
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 2; }));
    // An oversized broadcast cannot become an error reply: also a resync.
    CHECK_FALSE(f.relay->send(
        {{"t", "msg"}, {"to", "*"}, {"msg", {{"type", "state"}, {"blob", std::string(1100000, 'x')}}}}, false));
    REQUIRE(f.hub.waitFor([&] { return f.hub.opened == 3; }));
}

TEST_CASE("relay client + server: remote request round trip through bindRelay", "[relay][protocol]") {
    ServerFixture s;
    FakeHub hub;
    PostQueue posts;
    const fs::path dir = tempDir("bind");
    auto fake = std::make_shared<test::FakeCloud>();
    cloud::CloudClient client(fake, test::kFakeBase, dir / "cloud.json");
    client.load();
    std::string err;
    REQUIRE(client.storeLink(test::kFakeToken, {"17", "Max", ""}, err));
    cloud::RelayClient::Options o;
    o.factory = hub.factory();
    o.linkPollMs = 10;
    cloud::RelayClient relay(client, "1.0.0", o);
    cloud::bindRelay(relay, *s.server, posts.post());
    REQUIRE(hub.waitFor([&] { return hub.opened == 1; }));
    hub.withCurrent([](FakeSocket& sock) {
        sock.serverSend({{"t", "controller"}, {"cid", "ipad"}, {"joined", true}});
        sock.serverSend({{"t", "msg"}, {"from", "ipad"}, {"msg", {{"type", "list_presets"}, {"id", 4}}}});
        sock.serverSend({{"t", "msg"}, {"from", "ipad"}, {"msg", {{"type", "cloud_unlink"}, {"id", 5}}}});
    });
    posts.drain();
    CHECK(s.server->remoteCount() == 1);
    REQUIRE(hub.waitFor([&] { return hub.frames.size() >= 3; })); // hello + 2 replies
    const auto msgs = hub.framesOfType("msg");
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0]["to"] == "ipad");
    CHECK(msgs[0]["msg"]["type"] == "list_presets_ok");
    CHECK(msgs[1]["msg"]["code"] == "forbidden");
    CHECK(client.linked()); // the denied unlink did nothing
    hub.withCurrent([](FakeSocket& sock) { sock.serverClose(1001); });
    REQUIRE(hub.waitFor([&] { return hub.opened == 2; }));
    posts.drain();
    CHECK(s.server->remoteCount() == 0); // disconnect clears the virtual clients
    relay.stop();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------- WinHTTP socket against a local ixwebsocket server ----------------

TEST_CASE("winhttp websocket: headers, text both ways, server close code", "[relay][net]") {
    ix::initNetSystem();
    int port = 0;
    std::unique_ptr<ix::WebSocketServer> srv;
    std::mutex m;
    std::condition_variable cv;
    std::string auth, path, got;
    for (int p = 47391; p < 47400 && !srv; ++p) {
        auto s = std::make_unique<ix::WebSocketServer>(p, "127.0.0.1");
        s->disablePerMessageDeflate();
        s->setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws,
                                          const ix::WebSocketMessagePtr& msg) {
            std::lock_guard<std::mutex> lk(m);
            if (msg->type == ix::WebSocketMessageType::Open) {
                auto it = msg->openInfo.headers.find("Authorization");
                auth = it == msg->openInfo.headers.end() ? "" : it->second;
                path = msg->openInfo.uri;
                ws.sendText(R"({"t":"ping"})");
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                got = msg->str;
                if (got == "close-me") ws.close(4008, "overflow");
                else ws.sendText(std::string(200000, 'y')); // fragmented on the wire
            }
            cv.notify_all();
        });
        if (s->listen().first) {
            s->start();
            srv = std::move(s);
            port = p;
        }
    }
    if (!srv) SKIP("no free local port for the test server");

    std::mutex cm;
    std::condition_variable ccv;
    std::vector<std::string> texts;
    int closeCode = 0;
    {
        auto ws = platform::makeWinHttpWebSocket();
        REQUIRE(ws);
        platform::WsHandshake hs;
        hs.url = "ws://127.0.0.1:" + std::to_string(port) + "/keysynth/api/relay?role=host";
        hs.headers = {{"Authorization", "Bearer abc"}};
        hs.timeoutMs = 3000;
        const auto r = ws->open(
            hs,
            [&](std::string t) {
                std::lock_guard<std::mutex> lk(cm);
                texts.push_back(std::move(t));
                ccv.notify_all();
            },
            [&](int code, const std::string&) {
                std::lock_guard<std::mutex> lk(cm);
                closeCode = code;
                ccv.notify_all();
            });
        REQUIRE(r.ok);
        CHECK(r.httpStatus == 101);
        {
            std::unique_lock<std::mutex> lk(cm);
            REQUIRE(ccv.wait_for(lk, 3s, [&] { return !texts.empty(); }));
            CHECK(texts[0] == R"({"t":"ping"})");
        }
        {
            std::lock_guard<std::mutex> lk(m);
            CHECK(auth == "Bearer abc");
            CHECK(path == "/keysynth/api/relay?role=host");
        }
        REQUIRE(ws->send("hello"));
        {
            std::unique_lock<std::mutex> lk(cm);
            REQUIRE(ccv.wait_for(lk, 3s, [&] { return texts.size() >= 2; }));
            CHECK(texts[1].size() == 200000);
        }
        REQUIRE(ws->send("close-me"));
        {
            std::unique_lock<std::mutex> lk(cm);
            REQUIRE(ccv.wait_for(lk, 3s, [&] { return closeCode != 0; }));
            CHECK(closeCode == 4008);
        }
        CHECK_FALSE(ws->send("after close"));
    }
    // Upgrade refused -> httpStatus (an HTTP 404 from a non-WS path is fine for this check: no 101).
    {
        auto ws = platform::makeWinHttpWebSocket();
        platform::WsHandshake hs;
        hs.url = "http://127.0.0.1:1/nothing"; // nothing listens on port 1
        hs.timeoutMs = 2000;
        const auto r = ws->open(hs, [](std::string) {}, [](int, const std::string&) {});
        CHECK_FALSE(r.ok);
        CHECK(r.httpStatus == 0);
        CHECK_FALSE(r.error.empty());
    }
    srv->stop();
}
