// Control-server port conflicts (ARCHITECTURE §11 "Port conflicts"): classification of the listener table, the
// pre-bind check (ix's SO_REUSEADDR would let a second bind succeed on Windows), and retryListen() once the
// occupying listener is gone. Uses ephemeral ports only.
#include "control/ControlServer.h"
#include "control/PortConflict.h"
#include "control/ProtocolHandler.h"
#include "control/Session.h"
#include "core/AppPaths.h"
#include "core/Engine.h"
#include "platform/PortOwner.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

using namespace ks;
using platform::TcpListener;

namespace {

TcpListener listener(std::string addr, unsigned long pid, std::string image) {
    TcpListener l;
    l.address = std::move(addr);
    l.ipv6 = l.address.find(':') != std::string::npos;
    l.pid = pid;
    l.image = std::move(image);
    l.path = l.image.empty() ? std::string() : "C:\\x\\" + l.image;
    return l;
}

#if defined(_WIN32)
// A plain listening socket (no SO_REUSEADDR), like a node/ws mock server.
struct FakeListener {
    SOCKET s = INVALID_SOCKET;
    int port = 0;
    explicit FakeListener(const char* addr = "127.0.0.1") {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, addr, &a.sin_addr);
        a.sin_port = 0;
        REQUIRE(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        REQUIRE(listen(s, 4) == 0);
        int len = sizeof(a);
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    void close() {
        if (s != INVALID_SOCKET) closesocket(s);
        s = INVALID_SOCKET;
    }
    ~FakeListener() {
        close();
        WSACleanup();
    }
};

struct ServerFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("ks-port-test-" + std::to_string(platform::currentProcessId()) + "-" +
                                  std::to_string(counter()++));
    static int& counter() {
        static int n = 0;
        return n;
    }
    Engine engine;
    std::unique_ptr<Session> session;
    std::unique_ptr<ProtocolHandler> handler;
    std::unique_ptr<ControlServer> server;
    ServerFixture() {
        std::filesystem::create_directories(root);
        engine.prepare(48000.0, 64);
        session = std::make_unique<Session>(engine, defaultRegistry(), AppPaths::fromRoot(root));
        handler = std::make_unique<ProtocolHandler>(*session);
        server = std::make_unique<ControlServer>(*handler, [](std::function<void()> f) { f(); }, root / "no-dist");
    }
    ~ServerFixture() {
        server.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
#endif

} // namespace

TEST_CASE("port conflict: classification of listener tables", "[port]") {
    CHECK_FALSE(findConflict(7341, {}).has_value());
    // [::1] does not block 127.0.0.1, but is reported as a shadow (localhost clients may reach it)
    const std::vector<TcpListener> v6only = {listener("::1", 10, "node.exe")};
    CHECK_FALSE(findConflict(7341, v6only).has_value());
    CHECK(shadowListeners(v6only).size() == 1);

    for (const char* addr : {"127.0.0.1", "0.0.0.0", "::"}) {
        const auto c = findConflict(7341, {listener(addr, 1234, "node.exe")});
        REQUIRE(c);
        CHECK(c->kind == PortOwnerKind::Foreign);
        CHECK(c->pid == 1234);
        CHECK(c->name == "node.exe");
        CHECK(c->address == addr);
    }
    const auto k = findConflict(7341, {listener("127.0.0.1", 5, "node.exe"), listener("0.0.0.0", 77, "KeySynth-Engine.EXE")});
    REQUIRE(k);
    CHECK(k->kind == PortOwnerKind::Keysynth); // a keysynth owner wins: it decides "open its UI and exit"
    CHECK(k->pid == 77);

    PortConflict c;
    c.port = 7341;
    c.kind = PortOwnerKind::Foreign;
    c.pid = 1234;
    c.name = "node.exe";
    CHECK(conflictMessage(c, true) ==
          "Порт 7341 занят процессом node.exe (PID 1234). Закройте его — keysynth переподключится автоматически.");
    CHECK(conflictMessage(c, false) ==
          "Port 7341 is in use by node.exe (PID 1234). Close it and keysynth will reconnect automatically.");
    c.name.clear();
    CHECK(conflictMessage(c, true).find("неизвестным процессом (PID 1234)") != std::string::npos);
    c.kind = PortOwnerKind::Unknown;
    c.error = "bind failed";
    CHECK(conflictMessage(c, false) == "Port 7341 is unavailable (bind failed). keysynth retries every 3 s.");
    CHECK(portFreeMessage(7341, true) == "Порт 7341 свободен — движок снова доступен.");
}

#if defined(_WIN32)
TEST_CASE("port conflict: listener table names the owning process", "[port]") {
    FakeListener fake;
    const auto ls = platform::tcpListeners(fake.port);
    REQUIRE(ls.size() == 1);
    CHECK(ls[0].address == "127.0.0.1");
    CHECK(ls[0].pid == platform::currentProcessId());
    CHECK(ls[0].image == "ks-tests.exe");
    const auto c = findConflict(fake.port, ls);
    REQUIRE(c);
    CHECK(c->kind == PortOwnerKind::Foreign);
}

TEST_CASE("port conflict: control server refuses a busy port and binds after it is freed", "[port]") {
    FakeListener fake;
    ServerFixture f;
    const std::string err = f.server->start(fake.port, 0);
    REQUIRE_FALSE(err.empty());
    CHECK(err.find("ks-tests.exe") != std::string::npos);
    CHECK_FALSE(f.server->wsRunning());
    REQUIRE(f.server->wsConflict());
    CHECK(f.server->wsConflict()->kind == PortOwnerKind::Foreign);
    CHECK(f.server->wsConflict()->pid == platform::currentProcessId());

    CHECK_FALSE(f.server->retryListen()); // still busy
    CHECK_FALSE(f.server->wsRunning());

    fake.close();
    CHECK(f.server->retryListen());
    CHECK(f.server->wsRunning());
    CHECK_FALSE(f.server->wsConflict());
    CHECK_FALSE(f.server->retryListen()); // nothing left to bind
}

TEST_CASE("port conflict: a second control server on the same port is refused (no SO_REUSEADDR hijack)", "[port]") {
    FakeListener probe; // grab a free port number, then release it
    const int port = probe.port;
    probe.close();
    ServerFixture a, b;
    REQUIRE(a.server->start(port, 0).empty());
    const std::string err = b.server->start(port, 0);
    REQUIRE_FALSE(err.empty());
    REQUIRE(b.server->wsConflict());
    CHECK(b.server->wsConflict()->pid == platform::currentProcessId());
    a.server->stop();
    CHECK(b.server->retryListen());
    CHECK(b.server->wsRunning());
}

TEST_CASE("port conflict: wildcard IPv4 listener blocks the loopback bind", "[port]") {
    FakeListener fake("0.0.0.0");
    ServerFixture f;
    REQUIRE_FALSE(f.server->start(fake.port, 0).empty());
    REQUIRE(f.server->wsConflict());
    CHECK(f.server->wsConflict()->address == "0.0.0.0");
}
#endif
