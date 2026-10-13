#include "cloud/RelayClient.h"

#include "cloud/CloudClient.h"
#include "control/ControlServer.h"

#include <algorithm>
#include <cstdio>
#include <random>

using nlohmann::json;

namespace ks::cloud {

namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void logLine(const std::string& s) {
    std::printf("[relay] %s\n", s.c_str());
    std::fflush(stdout);
}
} // namespace

const char* RelayClient::toString(State s) {
    switch (s) {
    case State::Idle: return "idle";
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::Backoff: return "backoff";
    default: return "stopped";
    }
}

RelayClient::RelayClient(CloudClient& client, std::string version) : RelayClient(client, std::move(version), Options{}) {}

RelayClient::RelayClient(CloudClient& client, std::string version, Options opt)
    : client_(client), version_(std::move(version)), opt_(std::move(opt)) {
    if (!opt_.factory) opt_.factory = [] { return platform::makeDefaultWebSocket(); };
    random_ = opt_.random;
    if (!random_) {
        auto rng = std::make_shared<std::mt19937>(std::random_device{}());
        random_ = [rng] { return std::uniform_real_distribution<double>(0.0, 1.0)(*rng); };
    }
}

RelayClient::~RelayClient() { stop(); }

std::string RelayClient::relayUrl(const std::string& apiBase) {
    std::string b = apiBase;
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (b.rfind("https://", 0) == 0) b = "wss://" + b.substr(8);
    else if (b.rfind("http://", 0) == 0) b = "ws://" + b.substr(7);
    return b + "/api/relay?role=host";
}

int RelayClient::backoffMs(int failures, int minMs, int maxMs, double r) {
    double base = minMs;
    for (int i = 0; i < failures && base < maxMs; ++i) base *= 2.0;
    base = std::min<double>(base, maxMs);
    const double d = base * (0.8 + 0.4 * std::clamp(r, 0.0, 1.0));
    return static_cast<int>(std::clamp<double>(d, minMs, maxMs));
}

void RelayClient::start(Post post, Handlers handlers) {
    if (thread_.joinable()) return;
    post_ = std::move(post);
    handlers_ = std::move(handlers);
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = false;
    }
    if (!alive_) alive_ = std::make_shared<bool>(true);
    state_ = State::Idle;
    thread_ = std::thread([this] { run(); });
}

void RelayClient::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true; // run() leaves its wait, closes and destroys the socket
        if (opening_) opening_->abort(); // cancels a blocking handshake
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    alive_.reset(); // posted handlers that did not run yet become no-ops
    state_ = State::Stopped;
}

void RelayClient::wake() {
    {
        std::lock_guard<std::mutex> lk(m_);
        woken_ = true;
    }
    cv_.notify_all();
}

bool RelayClient::waitStop(int ms, bool wakeable) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(std::max(ms, 0)), [&] { return stop_ || (wakeable && woken_); });
    woken_ = false;
    return stop_;
}

void RelayClient::postLocked(std::function<void(const Handlers&)> f) {
    if (!post_) return;
    std::weak_ptr<bool> alive = alive_;
    const Handlers h = handlers_;
    post_([alive, h, f = std::move(f)] {
        if (alive.expired()) return;
        f(h);
    });
}

void RelayClient::postFor(uint64_t gen, std::function<void(const Handlers&)> f) {
    // Under m_: a frame of a connection that run() already retired (gen bumped + `cleared` posted) never posts.
    std::lock_guard<std::mutex> lk(m_);
    if (gen != gen_) return;
    postLocked(std::move(f));
}

bool RelayClient::sendRaw(const json& j) {
    const std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> lk(m_);
    return ws_ && !closed_ && ws_->send(text);
}

bool RelayClient::send(const json& envelope, bool lossy) {
    const std::string text = envelope.dump(-1, ' ', false, json::error_handler_t::replace);
    if (text.size() > opt_.maxFrameBytes) {
        logLine("dropped a " + std::to_string(text.size()) + "-byte frame (over the relay cap)");
        if (lossy) return false;
        // A reply that cannot be delivered becomes an error reply; a lost broadcast would leave the controllers
        // out of sync, so they are resynced by a reconnect (they re-hello).
        const auto& m = envelope.value("msg", json::object());
        const std::string to = envelope.value("to", std::string());
        if (to != "*" && m.is_object() && m.contains("id")) {
            send({{"t", "msg"},
                  {"to", to},
                  {"msg", {{"type", "error"}, {"id", m["id"]}, {"code", "too_large"},
                           {"message", "reply exceeds the relay frame limit"}}}},
                 false);
            return false; // the reply itself was dropped
        }
        requestResync();
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    if (!ws_ || closed_) return false;
    if (lossy && ws_->queuedBytes() > opt_.lossyQueueLimit) return false;
    if (ws_->send(text)) return true;
    if (!lossy) { // queue over its hard cap: only a reconnect brings the controllers back in sync
        resync_ = true;
        cv_.notify_all();
    }
    return false;
}

void RelayClient::requestResync() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!ws_) return;
        resync_ = true;
    }
    cv_.notify_all();
}

void RelayClient::onText(uint64_t gen, std::string text) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (gen != gen_) return; // a previous connection
    }
    auto postToMessageThread = [this, gen](std::function<void(const Handlers&)> f) { postFor(gen, std::move(f)); };
    lastRxMs_ = nowMs();
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    const std::string t = str(j, "t");
    if (t == "ping") {
        sendRaw({{"t", "pong"}});
    } else if (t == "pong") {
        // liveness only
    } else if (t == "msg") {
        std::string cid = str(j, "from");
        auto m = j.find("msg");
        if (cid.empty() || m == j.end()) return;
        json msg = *m;
        postToMessageThread([cid, msg = std::move(msg)](const Handlers& h) {
            if (h.message) h.message(cid, msg);
        });
    } else if (t == "controller") {
        const std::string cid = str(j, "cid");
        auto jn = j.find("joined");
        if (cid.empty() || jn == j.end() || !jn->is_boolean()) return;
        const bool joined = jn->get<bool>();
        postToMessageThread([cid, joined](const Handlers& h) {
            if (h.controller) h.controller(cid, joined);
        });
    } else if (t == "pause" || t == "resume") {
        std::string cid = str(j, "cid");
        if (cid.empty()) cid = str(j, "from");
        if (cid.empty()) return;
        const bool p = t == "pause";
        postToMessageThread([cid, p](const Handlers& h) {
            if (h.paused) h.paused(cid, p);
        });
    } else if (t == "controllers") {
        auto c = j.find("count");
        if (c != j.end() && c->is_number_integer() && c->get<int64_t>() == 0)
            postToMessageThread([](const Handlers& h) {
                if (h.cleared) h.cleared();
            });
    } else if (t == "error") {
        logLine("server error: " + j.value("code", json()).dump());
    }
}

void RelayClient::onClose(uint64_t gen, int code, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (gen != gen_) return;
        closed_ = true;
    }
    cv_.notify_all();
    logLine("closed (" + std::to_string(code) + (reason.empty() ? "" : " " + reason) + ")");
}

void RelayClient::run() {
    const std::string url = relayUrl(client_.baseUrl());
    std::string lastError;
    while (true) {
        const std::string token = client_.currentToken();
        if (token.empty()) {
            state_ = State::Idle;
            failures_ = 0;
            lastError.clear();
            if (waitStop(opt_.linkPollMs, true)) break;
            continue;
        }
        state_ = State::Connecting;
        std::unique_ptr<platform::WebSocket> ws = opt_.factory();
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stop_) break;
            gen = ++gen_;
            closed_ = false;
            resync_ = false;
            opening_ = ws.get();
        }
        platform::WsOpenResult res;
        if (ws) {
            platform::WsHandshake hs;
            hs.url = url;
            hs.headers = {{"Authorization", "Bearer " + token}};
            hs.timeoutMs = opt_.handshakeTimeoutMs;
            res = ws->open(
                hs, [this, gen](std::string text) { onText(gen, std::move(text)); },
                [this, gen](int code, const std::string& reason) { onClose(gen, code, reason); });
        } else {
            res.error = "no WebSocket implementation";
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            opening_ = nullptr;
            if (stop_) break; // (ws destroyed on scope exit)
        }
        if (!res.ok) {
            ws.reset();
            if (res.httpStatus == 401 || res.httpStatus == 403) {
                // Same as an API 401 (CloudClient): drop the token -> cloud_status { linked:false } via the pump.
                logLine("upgrade refused (HTTP " + std::to_string(res.httpStatus) + "): dropping the device token");
                client_.clearLinkIfToken(token, "token_invalid");
                failures_ = 0;
                continue;
            }
            if (res.error != lastError) logLine("connect failed: " + res.error);
            lastError = res.error;
            const int delay = backoffMs(failures_.load(), opt_.backoffMinMs, opt_.backoffMaxMs, random_());
            failures_.fetch_add(1);
            lastDelayMs_ = delay;
            state_ = State::Backoff;
            if (waitStop(delay, false)) break;
            continue;
        }
        // Connected.
        lastError.clear();
        connects_.fetch_add(1);
        const int64_t openedAt = nowMs();
        lastRxMs_ = openedAt;
        {
            std::lock_guard<std::mutex> lk(m_);
            ws_ = ws.get();
        }
        state_ = State::Connected;
        logLine("connected to " + url);
        sendRaw({{"t", "hello"},
                 {"host", {{"deviceId", client_.deviceId()}, {"name", client_.deviceName()}, {"version", version_}}}});
        int64_t lastPing = openedAt;
        bool stopping = false;
        while (true) {
            const int64_t now = nowMs();
            const int64_t untilPing = lastPing + opt_.pingIntervalMs - now;
            const int wait = static_cast<int>(std::clamp<int64_t>(untilPing, 1, opt_.linkPollMs));
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(wait),
                             [&] { return stop_ || closed_ || woken_ || resync_; });
                woken_ = false;
                if (stop_) {
                    stopping = true;
                    break;
                }
                if (closed_) break;
                if (resync_) {
                    lk.unlock();
                    logLine("a non-lossy frame could not be queued: reconnecting to resync the controllers");
                    ws->close(platform::kWsCloseGoingAway, "resync");
                    break;
                }
            }
            const int64_t t = nowMs();
            if (client_.currentToken() != token) { // unlinked or re-linked: reconnect with the new identity
                ws->close(platform::kWsCloseNormal, "relink");
                break;
            }
            if (t - lastRxMs_.load() > opt_.silenceTimeoutMs) {
                logLine("no frame for " + std::to_string(opt_.silenceTimeoutMs) + " ms: reconnecting");
                ws->close(platform::kWsCloseGoingAway, "timeout");
                break;
            }
            if (t - lastPing >= opt_.pingIntervalMs) {
                lastPing = t;
                sendRaw({{"t", "ping"}});
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            ws_ = nullptr;
            closed_ = true;
            ++gen_; // late frames of this socket are ignored from here on (they would re-join ghosts)
            postLocked([](const Handlers& h) {
                if (h.cleared) h.cleared();
            });
        }
        ws.reset(); // joins the socket threads
        if (stopping) break;
        if (nowMs() - openedAt >= opt_.stableMs) failures_ = 0;
        const int delay = backoffMs(failures_.load(), opt_.backoffMinMs, opt_.backoffMaxMs, random_());
        failures_.fetch_add(1);
        lastDelayMs_ = delay;
        state_ = State::Backoff;
        if (waitStop(delay, false)) break;
    }
    std::lock_guard<std::mutex> lk(m_);
    ws_ = nullptr;
}

void bindRelay(RelayClient& relay, ControlServer& server, RelayClient::Post post) {
    server.setRemoteSink([&relay](const json& env, bool lossy) { relay.send(env, lossy); });
    RelayClient::Handlers h;
    h.controller = [&server](const std::string& cid, bool joined) {
        if (joined) server.remoteJoined(cid);
        else server.remoteLeft(cid);
    };
    h.message = [&server](const std::string& cid, const json& msg) { server.remoteMessage(cid, msg); };
    h.paused = [&server](const std::string& cid, bool p) { server.remotePaused(cid, p); };
    h.cleared = [&server] { server.remoteClear(); };
    relay.start(std::move(post), std::move(h));
}

} // namespace ks::cloud
