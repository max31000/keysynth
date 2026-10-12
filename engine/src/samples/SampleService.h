#pragma once
// SampleService (ARCHITECTURE §15): sample cache + downloader. Contract: PROTOCOL.md "Cloud & samples" (jobs,
// samples_* messages), cloud-api spec §3.2 (resume), §4 (manifest, cache.json).
//
// Threads: protocol calls + poll() on the message thread; refreshManifest() on a worker (CloudClient's);
// `workers` download threads. One mutex guards all state; file downloads run without it, only the final
// rename + cache.json update ("commit") holds it. Never touches the audio thread.

#include "instruments/sampler/SamplePaths.h"
#include "samples/CacheIndex.h"
#include "samples/Manifest.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ks::cloud {
class CloudClient;
}

namespace ks::samples {

class SampleService {
public:
    struct Options {
        std::filesystem::path cacheRoot;
        std::vector<std::filesystem::path> localAssetDirs; // dirs laid out like assets/ (entry status `local`)
        int workers = 2;
        int maxRetries = 3;      // per file, after the first attempt
        int retryBaseMs = 1000;  // backoff 1 s, 2 s, 4 s
        int fileTimeoutMs = 30000;
    };

    SampleService(cloud::CloudClient& client, Options opt);
    ~SampleService();
    SampleService(const SampleService&) = delete;
    SampleService& operator=(const SampleService&) = delete;

    void start(); // load cache.json + manifest.json, start the download workers
    void stop();  // cancel transfers (keeps .part files), join workers

    // --- manifest ---
    // Revalidate the manifest (If-None-Match). Blocking: worker threads only. Returns "ok" | "stale" | "none".
    std::string refreshManifest(int timeoutMs);
    // Background revalidation, at most once per minute (link, startup). Worker threads only.
    void maybeRefreshManifest();
    std::string manifestState() const; // "ok" (fetched this run) | "stale" | "none"
    bool hasManifest() const;

    // --- protocol (message thread) ---
    // samples_list_ok payload (no type/id).
    nlohmann::json listJson(const std::string& manifestState) const;
    struct Result {
        std::string error, message; // error code ("" = ok)
        std::string job;
        bool existing = false;
        uint64_t freedBytes = 0;
        std::vector<std::string> jobs;
    };
    Result fetch(const std::string& scope, const std::string& target);
    Result cancel(const std::string& job); // "" = all
    // `inUse`: entry ids a sampler of the current patch uses (refused with in_use).
    Result remove(const std::string& scope, const std::string& target, const std::set<std::string>& inUse);

    // Sampler auto-fetch hook (SamplePaths.h), message thread. fresh = a new load request (prepare).
    SampleFetchState autoFetch(const std::string& entryId, bool fresh);
    // True (once per 30 s) when a sampler waits offline: the caller then runs refreshManifest() on the client
    // worker; its end wakes the samplers again (poll jobsEnded), which retries their downloads.
    bool wantsManifestRetry();

    // Pump (message thread): events to broadcast (samples_progress <= 5 Hz, samples_done, samples_error).
    // `jobsEnded` is set when a job ended since the last call (waiting samplers may now load).
    std::vector<nlohmann::json> poll(bool& jobsEnded);

    // Tests.
    bool idle() const; // no jobs
    const std::filesystem::path& cacheRoot() const noexcept { return opt_.cacheRoot; }

private:
    struct FileTask {
        std::string key, lib, path, sha256;
        uint64_t bytes = 0;
        bool valid = false;
        bool running = false;
        std::set<std::string> jobs;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<uint64_t>> done = std::make_shared<std::atomic<uint64_t>>(0);
    };
    struct Job {
        std::string id, scope, target;
        bool autoFetch = false;
        std::vector<std::string> entries;
        std::vector<std::string> files; // keys, fixed at start
        std::set<std::string> pending;
        uint64_t bytesTotal = 0, bytesFinished = 0;
        size_t filesTotal = 0, filesDone = 0;
        std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> rateWindow;
    };
    enum class Outcome { Ok, Cancelled, Failed };
    struct DownloadResult {
        Outcome outcome = Outcome::Failed;
        std::string code, message;
    };

    void workerLoop();
    DownloadResult download(const FileTask& t, const std::shared_ptr<std::atomic<bool>>& cancel,
                            const std::shared_ptr<std::atomic<uint64_t>>& done);
    void finishFile(const std::string& key, const DownloadResult& r); // locked
    void endJob(const std::string& jobId, bool cancelled, const std::string& code, const std::string& message,
                const std::string& path); // locked
    std::vector<std::string> targetEntries(const std::string& scope, const std::string& target) const; // locked
    bool isLocal(const std::string& lib, const std::string& sfz) const;
    std::string coveringJob(const std::string& lib, const std::string& entryId) const; // locked
    uint64_t jobBytesDone(const Job& j) const;                                           // locked
    nlohmann::json jobJson(const Job& j);                                                // locked
    void startJobLocked(Job j, bool front);
    bool sleepUnlessCancelled(int ms, const std::atomic<bool>& cancel);

    cloud::CloudClient& client_;
    Options opt_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    CacheIndex index_;
    std::shared_ptr<const Manifest> manifest_;
    bool manifestFresh_ = false;
    std::chrono::steady_clock::time_point lastManifestCheck_{};
    std::map<std::string, Job> jobs_;
    std::deque<std::string> order_;
    std::map<std::string, FileTask> tasks_;
    std::vector<nlohmann::json> events_;
    std::set<std::string> autoFailed_, autoCancelled_;
    std::set<std::string> offlineWaiters_; // entries a sampler waits for while the server is unreachable
    bool manifestAttempted_ = false;       // a catalog request finished (success or not) since start
    bool retryPosted_ = false;             // wantsManifestRetry() said yes, the refresh has not run yet
    bool jobsEnded_ = false;
    bool hadJobs_ = false;
    std::chrono::steady_clock::time_point lastProgress_{};
    bool stop_ = false;
    std::vector<std::thread> workers_;
};

} // namespace ks::samples
