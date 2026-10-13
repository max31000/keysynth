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
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace ks;
using platform::TcpListener;

namespace {

#if defined(_WIN32)
constexpr const char* kTestsImage = "ks-tests.exe";
#else
constexpr const char* kTestsImage = "ks-tests";
#endif

TcpListener listener(std::string addr, unsigned long pid, std::string image) {
    TcpListener l;
    l.address = std::move(addr);
    l.ipv6 = l.address.find(':') != std::string::npos;
    l.pid = pid;
    l.image = std::move(image);
    l.path = l.image.empty() ? std::string() : "C:\\x\\" + l.image;
    return l;
}

#if defined(_WIN32) || defined(__APPLE__)
// A plain listening socket (no SO_REUSEADDR), like a node/ws mock server.
struct FakeListener {
#if defined(_WIN32)
    using Socket = SOCKET;
    static constexpr Socket kNone = INVALID_SOCKET;
    static void closeSocket(Socket x) { closesocket(x); }
#else
    using Socket = int;
    static constexpr Socket kNone = -1;
    static void closeSocket(Socket x) { ::close(x); }
#endif
    Socket s = kNone;
    int port = 0;
    explicit FakeListener(const char* addr = "127.0.0.1") {
#if defined(_WIN32)
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, addr, &a.sin_addr);
        a.sin_port = 0;
        REQUIRE(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        REQUIRE(listen(s, 4) == 0);
        socklen_t len = sizeof(a);
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    void close() {
        if (s != kNone) closeSocket(s);
        s = kNone;
    }
    ~FakeListener() {
        close();
#if defined(_WIN32)
        WSACleanup();
#endif
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
    CHECK(isKeysynthImage("keysynth-engine")); // macOS (no extension)
    CHECK_FALSE(isKeysynthImage("keysynth-engine.app"));

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

#if defined(_WIN32) || defined(__APPLE__)
TEST_CASE("port conflict: listener table names the owning process", "[port]") {
    FakeListener fake;
    const auto ls = platform::tcpListeners(fake.port);
    REQUIRE(ls.size() == 1);
    CHECK(ls[0].address == "127.0.0.1");
    CHECK(ls[0].pid == platform::currentProcessId());
    CHECK(ls[0].image == kTestsImage);
    const auto c = findConflict(fake.port, ls);
    REQUIRE(c);
    CHECK(c->kind == PortOwnerKind::Foreign);
}

TEST_CASE("port conflict: control server refuses a busy port and binds after it is freed", "[port]") {
    FakeListener fake;
    ServerFixture f;
    const std::string err = f.server->start(fake.port, 0);
    REQUIRE_FALSE(err.empty());
    CHECK(err.find(kTestsImage) != std::string::npos);
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

#endif

// Windows bind semantics (a more specific 127.0.0.1 bind next to a wildcard / v6-only listener succeeds); BSD
// sockets refuse those binds, so on macOS the conflict is reported instead.
#if defined(_WIN32)
TEST_CASE("port conflict: a v6-only [::] listener does not block 127.0.0.1", "[port]") {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    DWORD v6only = 1;
    setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    REQUIRE(bind(s6, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    REQUIRE(listen(s6, 4) == 0);
    int len = sizeof(a);
    getsockname(s6, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin6_port);
    {
        ServerFixture f;
        CHECK_FALSE(ControlServer::probe(port).has_value());
        CHECK(f.server->start(port, 0).empty());
        CHECK(f.server->wsRunning());
    }
    closesocket(s6);
    WSACleanup();
}

TEST_CASE("port conflict: a plain 0.0.0.0 listener does not take 127.0.0.1 from us", "[port]") {
    // Windows lets the more specific 127.0.0.1 bind succeed and routes loopback connections to it, so the
    // exclusive trial bind passes and we serve; only an exclusive wildcard owner would block (then named).
    FakeListener fake("0.0.0.0");
    ServerFixture f;
    REQUIRE(f.server->start(fake.port, 0).empty());
    CHECK(f.server->wsRunning());
}
#endif
