#pragma once
// CloudServices (ARCHITECTURE §15): owns CloudClient, SampleService and SyncService, implements the cloud protocol
// messages (PROTOCOL.md "Cloud & samples") and switches Session profiles on link/unlink. Message thread API;
// results of background work are delivered by poll() (30 Hz pump) as addressed messages.

#include "cloud/CloudClient.h"
#include "cloud/SyncService.h"
#include "control/ProtocolHandler.h"
#include "samples/SampleService.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ks::cloud {

class CloudServices {
public:
    struct Options {
        std::string baseUrl = kDefaultApiBase;
        std::shared_ptr<HttpTransport> http; // default: JUCE/WinINet
        int syncIntervalMs = 60000;
        int downloadWorkers = 2;
        int retryBaseMs = 1000;
        bool sampleHook = true; // register the sampler auto-fetch hook + cache dir (process-wide)
    };

    CloudServices(Session& session, Options opt);
    ~CloudServices();
    CloudServices(const CloudServices&) = delete;
    CloudServices& operator=(const CloudServices&) = delete;

    void start(); // cloud.json, active profile, workers
    void stop();

    static bool handlesType(const std::string& type);
    // Throws PatchError (-> `error` reply). Synchronous replies go to `out`; async ones come out of poll().
    void handle(const std::string& type, const nlohmann::json& msg, const nlohmann::json& id, const ClientInfo& client,
                std::vector<Outgoing>& out);

    nlohmann::json cloudStatusJson() const;
    nlohmann::json syncStatusJson() const;
    static nlohmann::json offlineStatusJson();      // no cloud services: unlinked, no deviceId
    static nlohmann::json disabledSyncStatusJson(); // no cloud services

    // Pump (message thread): async replies + events (cloud_status, sync_status <= 2 Hz, samples_*).
    std::vector<AddressedOutgoing> poll();

    CloudClient& client() noexcept { return *client_; }
    samples::SampleService& samples() noexcept { return *samples_; }
    SyncService& sync() noexcept { return *sync_; }

private:
    using Continuation = std::function<void(std::vector<AddressedOutgoing>&)>;
    void later(Continuation c); // any thread -> run in poll()
    bool applyProfile();        // message thread: Session + SyncService follow the link; true if it changed
    nlohmann::json statusEvent(const nlohmann::json& id) const;

    Session& session_;
    Options opt_;
    std::unique_ptr<CloudClient> client_;
    std::unique_ptr<samples::SampleService> samples_;
    std::unique_ptr<SyncService> sync_;
    std::mutex laterM_;
    std::vector<Continuation> later_;
    std::string appliedAccount_;
    uint64_t lastCloudVersion_ = 0, lastSyncVersion_ = 0;
    std::chrono::steady_clock::time_point lastSyncEmit_{};
    bool started_ = false;
};

} // namespace ks::cloud
