#pragma once
// CloudClient (ARCHITECTURE §15): device identity + token (cloud.json, token DPAPI-protected; cloud-api spec §6.2),
// authenticated calls to the keysynth API, cloud status. Thread-safe (one mutex; never the audio thread).
// It has one worker thread for its own background calls (post()); api() may be called from any worker thread.

#include "cloud/HttpTransport.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ks::cloud {

inline constexpr const char* kDefaultApiBase = "https://mvv42.ru/keysynth";
inline constexpr const char* kTokenEntropy = "keysynth-cloud-v1";

struct LinkInfo {
    std::string accountId, accountName, expiresAt;
};

struct CloudStatus {
    bool linked = false;
    std::string accountId, accountName, deviceId, expiresAt;
    bool online = false;
    std::string error; // "" = null
    nlohmann::json toJson() const; // PROTOCOL.md CloudStatus
};

// cloud.json. Not thread-safe on its own (CloudClient guards it).
struct CloudStore {
    std::string deviceId, deviceName;
    std::optional<LinkInfo> link;
    std::string token; // decrypted, in memory only

    // Reads `file`; creates deviceId/deviceName when missing (then saves). A token that does not decrypt (other
    // Windows user / copied profile) is dropped: unlinked, deviceId kept.
    void load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const; // temp + rename
};

bool isValidToken(const std::string& token); // ^ksd_[A-Za-z0-9_-]{43}$
bool isUuidV4(const std::string& s);

class CloudClient {
public:
    CloudClient(std::shared_ptr<HttpTransport> http, std::string baseUrl, std::filesystem::path cloudFile);
    ~CloudClient();
    CloudClient(const CloudClient&) = delete;
    CloudClient& operator=(const CloudClient&) = delete;

    void load();         // cloud.json (message thread, at startup)
    void startWorker();  // background thread for post()
    void stopWorker();

    CloudStatus status() const;
    bool linked() const;
    std::string accountId() const;
    std::string deviceId() const;
    const std::string& baseUrl() const noexcept { return base_; }
    // Bumped on every status change (link, unlink, online <-> offline, error, token dropped).
    uint64_t statusVersion() const noexcept { return version_.load(); }

    // Authenticated call; req.url is a path below the API base ("/api/presets"). Worker threads only.
    // Network failure -> status 0 + online=false/"offline"; 401 -> the token is dropped (cloud_status event).
    HttpResponse api(HttpRequest req, const BodySink* sink = nullptr, const std::atomic<bool>* cancel = nullptr);
    // Same, but only while the link still uses `token` (a pass bound to one account must not continue with
    // another account's token after a re-link): otherwise status 0 + error "not_linked".
    HttpResponse apiAs(const std::string& token, HttpRequest req);

    // GET /api/devices/me with an explicit token (cloud_link step 2). Worker threads only. Does not touch state.
    struct MeResult {
        int status = 0;     // HTTP status (0 = offline)
        std::string error;  // "offline", "token_invalid", "http_error"
        LinkInfo info;
    };
    MeResult checkToken(const std::string& token);

    // Store a validated link (token DPAPI-protected into cloud.json). Message thread. False + err on failure.
    bool storeLink(const std::string& token, const LinkInfo& info, std::string& err);
    // Drop the token (unlink / 401). errorCode becomes CloudStatus.error ("" = null).
    void clearLink(const std::string& errorCode);
    // Drop the link only if it still uses `token` (401 on a request made with it). Atomic.
    bool clearLinkIfToken(const std::string& token, const std::string& errorCode);
    // Best-effort DELETE /api/devices/me with `token` (unlink). Worker threads only.
    void deleteDevice(const std::string& token);
    std::string currentToken() const;

    // Run `task` on the client's worker thread (FIFO).
    void post(std::function<void()> task);

private:
    void setOnline(bool online, const std::string& error);
    void workerLoop();

    std::shared_ptr<HttpTransport> http_;
    std::string base_;
    std::filesystem::path file_;
    mutable std::mutex m_;
    CloudStore store_;
    bool online_ = false;
    std::string error_;
    std::atomic<uint64_t> version_{0};

    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    bool started_ = false; // a worker ran once: after stop() tasks are dropped (never run inline)
    std::thread worker_;
};

} // namespace ks::cloud
