#pragma once
// ControlServer (ARCHITECTURE §11): WebSocket JSON on 127.0.0.1:<wsPort> (default 7341) + static HTTP for ui/dist
// on 127.0.0.1:<httpPort> (default 7340, only if ui/dist exists). Network threads never touch engine state:
// every message is posted to the message thread (`post`), where ProtocolHandler runs.

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

    // Returns "" on success.
    std::string start(int wsPort, int httpPort);
    void stop();
    bool httpRunning() const noexcept { return http_ != nullptr; }

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
    // Maps a request URI to a file inside distDir (SPA fallback to index.html). nullopt = 404/forbidden.
    static std::optional<std::filesystem::path> resolveStatic(const std::string& uri, const std::filesystem::path& distDir);

private:
    void onMessage(const ClientInfo& client, const std::string& text);
    void dispatch(const std::string& clientId, const std::vector<Outgoing>& out);

    ProtocolHandler& handler_;
    Post post_;
    std::filesystem::path uiDist_;
    int wsPort_ = 7341, httpPort_ = 7340;
    std::unique_ptr<ix::WebSocketServer> ws_;
    std::unique_ptr<ix::HttpServer> http_;
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
