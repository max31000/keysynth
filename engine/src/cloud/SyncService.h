#pragma once
// Preset + song sync (ARCHITECTURE §15, §18; algorithm: cloud-api spec §3.4, status: PROTOCOL.md `sync_status`).
// One SyncEngine per collection (presets, songs): same algorithm, own endpoint / body key / sync.json section.
// SyncEngine = one pass of the algorithm against an API function (testable with a fake transport);
// SyncService = the worker thread that runs passes on link, every 60 s, on local changes and on `sync_now`.
// Local file steps (read, write, rename) hold `fileMutex` (Session::profileMutex()), never across network calls.

#include "cloud/HttpTransport.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ks::cloud {

class CloudClient;

struct SyncConflict {
    std::string slug, file, at; // file: logical path "userdata/presets/<name>"
};

struct SyncBase {
    int64_t revision = 0;
    std::string hash; // SHA-256 of the local file at last sync; "" = absent locally (synced deletion)
};

struct SyncPassResult {
    bool ok = false;
    std::string error; // "offline", "unauthorized", "rate_limited", "http_error", "io_error", ... ("" = ok)
    int pushed = 0, pulled = 0, deleted = 0, conflicts = 0;
};

bool isValidSlug(const std::string& s); // ^[a-z0-9]+(-[a-z0-9]+)*$, 1..64

// What a SyncEngine syncs (cloud-api spec §3.3 presets, §3.3a songs).
struct SyncCollection {
    std::string endpoint = "/api/presets";        // REST collection URL
    std::string bodyKey = "patch";                // document key in PUT bodies / GET replies
    std::string baseKey = "presets";              // section of sync.json
    std::string logicalDir = "userdata/presets/"; // wire prefix of conflict files
    bool optional = false;                        // 404 on the list = server without this collection: skip
    static SyncCollection presets() { return {}; }
    static SyncCollection songs() { return {"/api/songs", "song", "songs", "userdata/songs/", true}; }
};

class SyncEngine {
public:
    using Api = std::function<HttpResponse(HttpRequest)>;
    SyncEngine(Api api, std::filesystem::path presetsDir, std::filesystem::path syncFile, std::mutex* fileMutex,
               SyncCollection collection = SyncCollection::presets());

    SyncPassResult runPass(const std::atomic<bool>* cancel = nullptr);
    // Local presets changed since their last sync (incl. new and deleted ones).
    int pendingCount() const;
    static std::vector<SyncConflict> conflicts(const std::filesystem::path& dir,
                                               const std::string& logicalDir = "userdata/presets/");
    std::string lastSyncAt() const;
    // Conflict file name for a slug at a UTC time ("<slug>.conflict-20261012T140300Z.json").
    static std::string conflictName(const std::string& slug, const std::string& compactUtc);

private:
    struct ServerMeta {
        int64_t revision = 0;
        bool deleted = false;
    };
    struct Step {
        bool ok = true;
        bool abort = false; // stop the pass (offline / unauthorized / rate limited)
        std::string error;
    };
    void loadBase();
    bool saveBase();
    std::map<std::string, std::string> scanLocal() const; // slug -> hash
    Step fail(const HttpResponse& r);
    Step download(const std::string& slug, SyncPassResult& res);
    Step upload(const std::string& slug, int64_t ifMatch, bool create, SyncPassResult& res, ServerMeta* conflict);
    Step remove(const std::string& slug, int64_t ifMatch, SyncPassResult& res, ServerMeta* conflict);
    Step conflictThenDownload(const std::string& slug, SyncPassResult& res);
    Step resolveServerChanged(const std::string& slug, const ServerMeta& s, SyncPassResult& res);
    std::filesystem::path fileOf(const std::string& slug) const { return dir_ / (slug + ".json"); }

    std::string url(const std::string& slug) const { return col_.endpoint + "/" + slug; }

    Api api_;
    SyncCollection col_;
    std::filesystem::path dir_, syncFile_;
    std::mutex* fileMutex_;
    std::map<std::string, SyncBase> base_;
    std::string lastSyncAt_;
};

class SyncService {
public:
    SyncService(CloudClient& client, std::mutex& fileMutex, int intervalMs = 60000);
    ~SyncService();

    void start();
    void stop();
    // Profile to sync: account presets dir + its sync.json (+ songs dir, empty = presets only). Empty = disabled.
    // Triggers a pass.
    void setProfile(const std::filesystem::path& presetsDir, const std::filesystem::path& syncFile,
                    const std::filesystem::path& songsDir = {});
    void requestSync();      // sync_now, local change
    void notifyLocalChange(); // recompute `pending` and schedule a pass
    nlohmann::json statusJson() const; // sync_status payload (no type)
    uint64_t statusVersion() const noexcept { return version_.load(); }
    bool enabled() const;

private:
    void loop();
    void setState(const std::string& state, const std::string& error);

    CloudClient& client_;
    std::mutex& fileMutex_;
    int intervalMs_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::filesystem::path dir_, syncFile_, songsDir_;
    uint64_t profileGen_ = 0;
    bool requested_ = false, stop_ = false;
    std::string state_ = "disabled", error_, lastSyncAt_;
    int pending_ = 0;
    std::vector<SyncConflict> conflicts_;
    std::atomic<uint64_t> version_{0};
    std::atomic<bool> cancelPass_{false};
    std::thread worker_;
};

} // namespace ks::cloud
