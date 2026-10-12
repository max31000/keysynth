#pragma once
// ControlServer (ARCHITECTURE §11): WebSocket JSON on 127.0.0.1:<wsPort> (default 7341) + static HTTP for ui/dist
// on 127.0.0.1:<httpPort> (default 7340, only if ui/dist exists). Network threads never touch engine state:
// every message is posted to the message thread (`post`), where ProtocolHandler runs.
// Port conflicts (§11): each bind is preceded by a listener-table check; a busy port is reported as a
// PortConflict and can be retried with retryListen().

#include "control/PortConflict.h"
#include "control/ProtocolHandler.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ix {
class WebSocket;
class WebSocketServer;
class HttpServer;
} // namespace ix

namespace ks {

class ControlServer {
public:
    using Post = std::function<void(std::function<void()>)>;

    ControlServer(ProtocolHandler& handler, Post post, std::filesystem::path uiDist);
    ~ControlServer();

    // Binds the WS port and (when ui/dist exists and httpPort > 0) the HTTP port, independently. Returns "" when
    // the WS port is bound, else the English conflict message (wsConflict() has the details). A busy HTTP port is
    // not an error (httpConflict()). Message thread.
    std::string start(int wsPort, int httpPort);
    // Binds whatever start() could not (WS, then HTTP). True when something got bound. Message thread.
    bool retryListen();
    void stop();
    bool wsRunning() const noexcept { return ws_ != nullptr; }
    bool httpRunning() const noexcept { return http_ != nullptr; }
    bool httpWanted() const; // httpPort > 0 and ui/dist/index.html exists
    const std::optional<PortConflict>& wsConflict() const noexcept { return wsConflict_; }
    const std::optional<PortConflict>& httpConflict() const noexcept { return httpConflict_; }
    int wsPort() const noexcept { return wsPort_; }
    int httpPort() const noexcept { return httpPort_; }

    // Origin policy (installed builds: devBuild = false). Set before start().
    void setOriginPolicy(OriginPolicy p) { policy_ = std::move(p); }

    // Message thread.
    void broadcast(const nlohmann::json& msg);
    // Deliver messages produced asynchronously (CloudServices::poll()).
    void dispatchAsync(const std::vector<AddressedOutgoing>& out);
    void sendTo(const std::string& clientId, const nlohmann::json& msg);
    void sendOthers(const std::string& exceptId, const nlohmann::json& msg);
    size_t clientCount() const;

    // Exposed for tests.
    static bool originAllowed(const std::string& origin, int wsPort, int httpPort); // dev policy
    // Handshake check: Host (DNS rebinding) + Origin. nullopt = refuse.
    static std::optional<Trust> admit(const std::string& host, const std::string& origin, int listenPort, int wsPort,
                                      int httpPort, const OriginPolicy& policy);
    static bool wsPathAllowed(const std::string& uri);
    // Pre-bind check of 127.0.0.1:<port>: trial exclusive bind, owner named from the listener table. nullopt = free.
    static std::optional<PortConflict> probe(int port);
    // Maps a request URI to a file inside distDir (SPA fallback to index.html). nullopt = 404/forbidden.
    static std::optional<std::filesystem::path> resolveStatic(const std::string& uri, const std::filesystem::path& distDir);

private:
    std::optional<PortConflict> listenWs();
    std::optional<PortConflict> listenHttp();
    void onMessage(const ClientInfo& client, const std::string& text);
    void dispatch(const std::string& clientId, const std::vector<Outgoing>& out);

    ProtocolHandler& handler_;
    Post post_;
    std::filesystem::path uiDist_;
    int wsPort_ = 7341, httpPort_ = 7340;
    std::unique_ptr<ix::WebSocketServer> ws_;
    std::unique_ptr<ix::HttpServer> http_;
    std::optional<PortConflict> wsConflict_, httpConflict_;
    mutable std::mutex clientsMutex_; // network threads + message thread (never the audio thread)
    struct Client {
        std::weak_ptr<ix::WebSocket> ws;
        Trust trust = Trust::Local;
    };
    std::map<std::string, Client> clients_;
    OriginPolicy policy_;
    std::shared_ptr<bool> alive_;
};

} // namespace ks
