#include "control/ControlServer.h"

#include "core/AppPaths.h"

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace ks {

namespace {
std::string mimeFor(const fs::path& p) {
    static const std::map<std::string, std::string> m = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".json", "application/json"}, {".svg", "image/svg+xml"}, {".png", "image/png"},
        {".ico", "image/x-icon"}, {".woff2", "font/woff2"}, {".woff", "font/woff"},
        {".map", "application/json"}, {".txt", "text/plain; charset=utf-8"}, {".wasm", "application/wasm"}};
    auto it = m.find(p.extension().string());
    return it == m.end() ? "application/octet-stream" : it->second;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}
} // namespace

ControlServer::ControlServer(ProtocolHandler& handler, Post post, fs::path uiDist)
    : handler_(handler), post_(std::move(post)), uiDist_(std::move(uiDist)), alive_(std::make_shared<bool>(true)) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::originAllowed(const std::string& origin, int wsPort, int httpPort) {
    // Non-browser clients (scripts, tests) send no Origin; browsers always do. Local processes are trusted anyway.
    if (origin.empty()) return true;
    static const std::regex re(R"(^http://(localhost|127\.0\.0\.1)(:(\d+))?$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(origin, m, re)) return false;
    if (!m[3].matched) return false;
    const int port = std::stoi(m[3].str());
    return port == wsPort || port == httpPort || port == 5173;
}

bool ControlServer::wsPathAllowed(const std::string& uri) {
    const std::string path = uri.substr(0, uri.find('?'));
    return path.empty() || path == "/" || path == "/ws";
}

std::optional<fs::path> ControlServer::resolveStatic(const std::string& uri, const fs::path& distDir) {
    std::string path = urlDecode(uri.substr(0, uri.find_first_of("?#")));
    if (path.find('\0') != std::string::npos || path.find('\\') != std::string::npos) return std::nullopt;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.empty()) path = "index.html";
    fs::path candidate = (distDir / pathFromUtf8(path)).lexically_normal();
    if (!isPathInside(candidate, distDir)) return std::nullopt;
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
    // SPA fallback for extension-less routes.
    if (!candidate.has_extension() && fs::is_regular_file(distDir / "index.html", ec)) return distDir / "index.html";
    return std::nullopt;
}

std::string ControlServer::start(int wsPort, int httpPort) {
    ix::initNetSystem();
    wsPort_ = wsPort;
    httpPort_ = httpPort;
    ws_ = std::make_unique<ix::WebSocketServer>(wsPort, "127.0.0.1");
    ws_->disablePerMessageDeflate();
    std::weak_ptr<bool> alive = alive_;
    ws_->setOnConnectionCallback([this, alive](std::weak_ptr<ix::WebSocket> wsWeak,
                                               std::shared_ptr<ix::ConnectionState> state) {
        auto ws = wsWeak.lock();
        if (!ws) return;
        const std::string id = state->getId();
        ws->setOnMessageCallback([this, alive, wsWeak, id](const ix::WebSocketMessagePtr& msg) {
            if (alive.expired()) return;
            switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                auto origin = msg->openInfo.headers.find("Origin");
                const std::string o = origin == msg->openInfo.headers.end() ? std::string() : origin->second;
                if (!originAllowed(o, wsPort_, httpPort_) || !wsPathAllowed(msg->openInfo.uri)) {
                    if (auto s = wsWeak.lock()) s->close(4003, "forbidden");
                    return;
                }
                std::lock_guard<std::mutex> lk(clientsMutex_);
                clients_[id] = wsWeak;
                break;
            }
            case ix::WebSocketMessageType::Close: {
                std::lock_guard<std::mutex> lk(clientsMutex_);
                clients_.erase(id);
                break;
            }
            case ix::WebSocketMessageType::Message: {
                {
                    std::lock_guard<std::mutex> lk(clientsMutex_);
                    if (!clients_.count(id)) return; // rejected connection
                }
                if (msg->binary) return;
                const std::string text = msg->str;
                post_([this, alive, id, text] {
                    if (alive.expired()) return;
                    onMessage(id, text);
                });
                break;
            }
            default: break;
            }
        });
    });
    auto res = ws_->listen();
    if (!res.first) {
        ws_.reset();
        return "WebSocket listen on 127.0.0.1:" + std::to_string(wsPort) + " failed: " + res.second;
    }
    ws_->start();

    std::error_code ec;
    if (httpPort > 0 && fs::is_regular_file(uiDist_ / "index.html", ec)) {
        http_ = std::make_unique<ix::HttpServer>(httpPort, "127.0.0.1");
        const fs::path dist = uiDist_;
        http_->setOnConnectionCallback([dist](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>) {
            ix::WebSocketHttpHeaders headers;
            if (req->method != "GET" && req->method != "HEAD")
                return std::make_shared<ix::HttpResponse>(405, "Method Not Allowed", ix::HttpErrorCode::Ok, headers, "");
            auto file = resolveStatic(req->uri, dist);
            if (!file) return std::make_shared<ix::HttpResponse>(404, "Not Found", ix::HttpErrorCode::Ok, headers, "not found");
            std::ifstream f(*file, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            headers["Content-Type"] = mimeFor(*file);
            headers["Cache-Control"] = "no-cache";
            return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok, headers,
                                                      req->method == "HEAD" ? std::string() : ss.str());
        });
        auto hr = http_->listen();
        if (!hr.first) http_.reset(); // non-fatal: UI can still be served by the Vite dev server
        else http_->start();
    }
    return {};
}

void ControlServer::stop() {
    alive_.reset();
    if (http_) {
        http_->stop();
        http_.reset();
    }
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
    std::lock_guard<std::mutex> lk(clientsMutex_);
    clients_.clear();
    alive_ = std::make_shared<bool>(true);
}

void ControlServer::onMessage(const std::string& clientId, const std::string& text) {
    dispatch(clientId, handler_.handleText(text));
}

void ControlServer::dispatch(const std::string& clientId, const std::vector<Outgoing>& out) {
    for (const auto& o : out) {
        switch (o.target) {
        case Outgoing::Target::Reply: sendTo(clientId, o.message); break;
        case Outgoing::Target::Broadcast: broadcast(o.message); break;
        case Outgoing::Target::Others: sendOthers(clientId, o.message); break;
        }
    }
}

void ControlServer::broadcast(const nlohmann::json& msg) { sendOthers({}, msg); }

void ControlServer::sendOthers(const std::string& exceptId, const nlohmann::json& msg) {
    std::vector<std::shared_ptr<ix::WebSocket>> targets;
    {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        for (auto& [id, w] : clients_)
            if (id != exceptId)
                if (auto s = w.lock()) targets.push_back(std::move(s));
    }
    if (targets.empty()) return;
    const std::string text = msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    for (auto& s : targets) s->sendText(text);
}

void ControlServer::sendTo(const std::string& clientId, const nlohmann::json& msg) {
    std::shared_ptr<ix::WebSocket> s;
    {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        auto it = clients_.find(clientId);
        if (it != clients_.end()) s = it->second.lock();
    }
    if (s) s->sendText(msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

size_t ControlServer::clientCount() const {
    std::lock_guard<std::mutex> lk(clientsMutex_);
    return clients_.size();
}

} // namespace ks
