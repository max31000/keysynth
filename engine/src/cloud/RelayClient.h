#pragma once
// RelayClient (ARCHITECTURE §15 *Relay*, spec docs/specs/2026-10-12-remote.md §10): the engine as relay host. While
// the engine is linked it keeps one outbound WebSocket to `<api root>/api/relay?role=host` (bearer device token),
// sends `hello`, keeps the app-level heartbeat and reconnects with backoff. Inbound controller events are posted to
// the message thread (Handlers); outbound envelopes come from ControlServer's RemoteRouter via send().
// Threads: one own thread (connect / backoff / heartbeat) + the socket's receive/send threads. Never the audio thread.

#include "platform/WinHttpWebSocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ks {
class ControlServer;
}

namespace ks::cloud {

class CloudClient;

class RelayClient {
public:
    using Post = std::function<void(std::function<void()>)>;

    struct Options {
        platform::WebSocketFactory factory; // default: WinHTTP
        int backoffMinMs = 1000, backoffMaxMs = 30000;
        int pingIntervalMs = 20000;   // app-level {t:"ping"}
        int silenceTimeoutMs = 45000; // no frame for this long -> reconnect
        int linkPollMs = 1000;        // re-check of the link / token while idle or connected
        int handshakeTimeoutMs = 10000;
        int stableMs = 10000;            // a connection that lived this long resets the backoff
        size_t maxFrameBytes = 1u << 20; // host -> server cap (spec §10.9)
        size_t lossyQueueLimit = 256u << 10; // lossy frames are dropped above this socket backlog
        std::function<double()> random;  // jitter source in [0,1) (tests); default: mt19937
    };

    // Invoked on the message thread (through Post).
    struct Handlers {
        std::function<void(const std::string& cid, bool joined)> controller;
        std::function<void(const std::string& cid, const nlohmann::json& msg)> message;
        std::function<void(const std::string& cid, bool paused)> paused;
        std::function<void()> cleared; // disconnected or zero controllers: every remote client is gone
    };

    enum class State { Stopped, Idle, Connecting, Connected, Backoff };
    static const char* toString(State s);

    RelayClient(CloudClient& client, std::string version, Options opt = {});
    ~RelayClient();
    RelayClient(const RelayClient&) = delete;
    RelayClient& operator=(const RelayClient&) = delete;

    void start(Post post, Handlers handlers);
    void stop();
    // The link may have changed (CloudServices on every status change): re-check now instead of at the next poll.
    void wake();

    // Any thread: queue an envelope ({t:"msg", ...}). False when not connected or dropped (size / lossy backlog).
    bool send(const nlohmann::json& envelope, bool lossy);

    State state() const noexcept { return state_.load(); }
    int connects() const noexcept { return connects_.load(); }    // successful upgrades
    int failures() const noexcept { return failures_.load(); }    // consecutive failed attempts
    int lastDelayMs() const noexcept { return lastDelayMs_.load(); } // last backoff wait

    static std::string relayUrl(const std::string& apiBase); // https://h/keysynth -> wss://h/keysynth/api/relay?role=host
    // Backoff before attempt `failures` (0-based): min(max, min * 2^failures) scaled by 0.8 + 0.4 * r, clamped.
    static int backoffMs(int failures, int minMs, int maxMs, double r);

private:
    void run();
    void onText(uint64_t gen, std::string text);
    void onClose(uint64_t gen, int code, const std::string& reason);
    bool sendRaw(const nlohmann::json& j);
    void postToMessageThread(std::function<void(const Handlers&)> f);
    bool waitStop(int ms, bool wakeable); // true when stopping

    CloudClient& client_;
    std::string version_;
    Options opt_;
    Post post_;
    Handlers handlers_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false, woken_ = false, closed_ = false;
    platform::WebSocket* ws_ = nullptr; // the connected socket (owned by run())
    uint64_t gen_ = 0;
    std::atomic<int64_t> lastRxMs_{0};
    std::thread thread_;
    std::atomic<State> state_{State::Stopped};
    std::atomic<int> connects_{0}, failures_{0}, lastDelayMs_{0};
    std::function<double()> random_;
};

// Wire a RelayClient to the ControlServer's remote clients (handlers + outbound sink) and start it.
void bindRelay(RelayClient& relay, ControlServer& server, RelayClient::Post post);

} // namespace ks::cloud
