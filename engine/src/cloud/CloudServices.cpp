#include "cloud/CloudServices.h"

#include "core/AppPaths.h"
#include "core/PatchModel.h"
#include "instruments/sampler/SamplePaths.h"

#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ks::cloud {

namespace {
std::string strOpt(const json& m, const char* key) {
    auto it = m.find(key);
    if (it == m.end() || it->is_null()) return {};
    if (!it->is_string()) throw PatchError("bad_request", std::string("field '") + key + "' must be a string");
    return it->get<std::string>();
}

// Copy *.json of `from` into `to` (no overwrite). Returns the number copied.
int copyJsonFiles(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return 0;
    fs::create_directories(to, ec);
    int n = 0;
    for (const auto& e : fs::directory_iterator(from, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
        const fs::path dst = to / e.path().filename();
        if (fs::exists(dst, ec)) continue;
        if (fs::copy_file(e.path(), dst, ec) && !ec) ++n;
    }
    return n;
}
} // namespace

CloudServices::CloudServices(Session& session, Options opt) : session_(session), opt_(std::move(opt)) {
    if (!opt_.http) opt_.http = makeJuceHttpTransport();
    const AppPaths& p = session_.paths();
    client_ = std::make_unique<CloudClient>(opt_.http, opt_.baseUrl, p.cloudFile);
    samples::SampleService::Options so;
    so.cacheRoot = p.sampleCache;
    so.localAssetDirs = sampleAssetDirs(p.root);
    so.workers = opt_.downloadWorkers;
    so.retryBaseMs = opt_.retryBaseMs;
    samples_ = std::make_unique<samples::SampleService>(*client_, so);
    sync_ = std::make_unique<SyncService>(*client_, session_.profileMutex(), opt_.syncIntervalMs);
    relay_ = std::make_unique<RelayClient>(*client_, engineVersion(), opt_.relay);
}

CloudServices::~CloudServices() { stop(); }

void CloudServices::start() {
    if (started_) return;
    started_ = true;
    client_->load();
    client_->startWorker();
    samples_->start();
    sync_->start();
    if (opt_.sampleHook) {
        setSampleCacheDir(session_.paths().sampleCache);
        setSampleFetchHook([this](const std::string& entry, bool fresh) { return samples_->autoFetch(entry, fresh); });
    }
    session_.onUserPresetsChanged = [this] { sync_->notifyLocalChange(); };
    applyProfile();
    lastCloudVersion_ = client_->statusVersion();
    if (client_->linked()) client_->post([this] { samples_->maybeRefreshManifest(); });
}

void CloudServices::stop() {
    relay_->stop(); // started separately (bindRelay)
    if (!started_) return;
    started_ = false;
    if (opt_.sampleHook) setSampleFetchHook({});
    session_.onUserPresetsChanged = nullptr;
    client_->stopWorker();
    sync_->stop();
    samples_->stop();
}

bool CloudServices::handlesType(const std::string& t) {
    static const std::set<std::string> types = {"cloud_link",    "cloud_unlink",  "sync_now",      "samples_list",
                                                "samples_fetch", "samples_cancel", "samples_delete"};
    return types.count(t) > 0;
}

json CloudServices::cloudStatusJson() const { return client_->status().toJson(); }
json CloudServices::syncStatusJson() const { return sync_->statusJson(); }

json CloudServices::offlineStatusJson() {
    CloudStatus s;
    return s.toJson();
}

json CloudServices::disabledSyncStatusJson() {
    return {{"state", "disabled"}, {"lastSyncAt", nullptr}, {"pending", 0}, {"conflicts", json::array()}, {"error", nullptr}};
}

json CloudServices::statusEvent(const json& id) const {
    json j = cloudStatusJson();
    j["type"] = "cloud_status";
    if (!id.is_null()) j["id"] = id;
    return j;
}

void CloudServices::later(Continuation c) {
    std::lock_guard<std::mutex> lk(laterM_);
    later_.push_back(std::move(c));
}

bool CloudServices::applyProfile() {
    const std::string account = client_->accountId();
    if (account == appliedAccount_ && (account.empty() || sync_->enabled())) return false;
    const bool changed = account != appliedAccount_;
    if (!session_.setProfile(account)) return false;
    appliedAccount_ = account;
    const AppPaths& p = session_.paths();
    if (account.empty()) sync_->setProfile({}, {});
    else sync_->setProfile(p.userPresets, p.userdata / "sync.json", p.userSongs);
    return changed;
}

void CloudServices::handle(const std::string& type, const json& msg, const json& id, const ClientInfo& client,
                           std::vector<Outgoing>& out) {
    auto reply = [&](json m) {
        if (!id.is_null()) m["id"] = id;
        out.push_back({Outgoing::Target::Reply, std::move(m)});
    };
    const std::string clientId = client.id;

    if (type == "cloud_link") {
        const std::string token = strOpt(msg, "token");
        bool replace = false;
        if (auto it = msg.find("replace"); it != msg.end() && it->is_boolean()) replace = it->get<bool>();
        if (!isValidToken(token)) throw PatchError("bad_request", "malformed device token");
        client_->post([this, token, replace, id, clientId] {
            const CloudClient::MeResult me = client_->checkToken(token);
            later([this, me, token, replace, id, clientId](std::vector<AddressedOutgoing>& q) {
                auto err = [&](const std::string& code, const std::string& message) {
                    q.push_back({clientId, {Outgoing::Target::Reply, ProtocolHandler::error(id, code, message)}});
                };
                if (!me.error.empty()) {
                    err(me.error, me.error == "offline" ? "cannot reach the keysynth server"
                                  : me.error == "token_invalid" ? "the server rejected the token"
                                                                : "unexpected server reply (HTTP " + std::to_string(me.status) + ")");
                    return;
                }
                const CloudStatus cur = client_->status();
                if (cur.linked && cur.accountId != me.info.accountId && !replace) {
                    err("account_mismatch", "this engine is linked to account '" +
                                                (cur.accountName.empty() ? cur.accountId : cur.accountName) +
                                                "'; resend with replace: true to switch");
                    return;
                }
                const fs::path accountDir = session_.paths().accountProfileDir(me.info.accountId);
                std::error_code ec;
                const bool firstLink = !fs::exists(accountDir, ec);
                std::string serr;
                if (!client_->storeLink(token, me.info, serr)) {
                    err("io_error", serr);
                    return;
                }
                json merged;
                if (firstLink) {
                    const AppPaths& p = session_.paths();
                    fs::create_directories(accountDir, ec);
                    const int presets = copyJsonFiles(p.localProfile / "presets", accountDir / "presets");
                    const int patterns = copyJsonFiles(p.localProfile / "patterns", accountDir / "patterns");
                    const int songs = copyJsonFiles(p.localProfile / "songs", accountDir / "songs");
                    merged = {{"presets", presets}, {"patterns", patterns}, {"songs", songs}};
                }
                const bool switched = applyProfile();
                if (!switched) sync_->requestSync();
                json st = statusEvent(id);
                if (!merged.is_null()) st["merged"] = merged;
                q.push_back({clientId, {Outgoing::Target::Reply, std::move(st)}});
                q.push_back({clientId, {Outgoing::Target::Others, statusEvent(nullptr)}});
                if (switched) q.push_back({clientId, {Outgoing::Target::Broadcast, session_.stateJson()}});
                if (switched) q.push_back({clientId, {Outgoing::Target::Broadcast, session_.songStateJson()}});
                lastCloudVersion_ = client_->statusVersion();
                relay_->wake();
                client_->post([this] { samples_->maybeRefreshManifest(); });
            });
        });
        return;
    }
    if (type == "cloud_unlink") {
        const std::string token = client_->currentToken();
        client_->clearLink("");
        if (!token.empty()) client_->post([this, token] { client_->deleteDevice(token); });
        const bool switched = applyProfile();
        reply(statusEvent(nullptr));
        out.push_back({Outgoing::Target::Others, statusEvent(nullptr)});
        if (switched) out.push_back({Outgoing::Target::Broadcast, session_.stateJson()});
        if (switched) out.push_back({Outgoing::Target::Broadcast, session_.songStateJson()});
        lastCloudVersion_ = client_->statusVersion();
        relay_->wake();
        return;
    }
    if (type == "sync_now") {
        if (!client_->linked()) throw PatchError("not_linked", "link this engine to your account first");
        sync_->requestSync();
        reply({{"type", "sync_now_ok"}});
        return;
    }
    if (type == "samples_list") {
        if (!client_->linked()) {
            json r = samples_->listJson(samples_->hasManifest() ? "stale" : "none");
            r["type"] = "samples_list_ok";
            reply(std::move(r));
            return;
        }
        client_->post([this, id, clientId] {
            const std::string state = samples_->refreshManifest(5000);
            later([this, state, id, clientId](std::vector<AddressedOutgoing>& q) {
                json r = samples_->listJson(state);
                r["type"] = "samples_list_ok";
                if (!id.is_null()) r["id"] = id;
                q.push_back({clientId, {Outgoing::Target::Reply, std::move(r)}});
            });
        });
        return;
    }
    if (type == "samples_fetch") {
        const auto r = samples_->fetch(strOpt(msg, "scope"), strOpt(msg, "target"));
        if (!r.error.empty()) throw PatchError(r.error, r.message);
        reply({{"type", "samples_fetch_ok"}, {"job", r.job}, {"existing", r.existing}});
        return;
    }
    if (type == "samples_cancel") {
        const auto r = samples_->cancel(strOpt(msg, "job"));
        reply({{"type", "samples_cancel_ok"}, {"jobs", r.jobs}});
        return;
    }
    if (type == "samples_delete") {
        const auto r = samples_->remove(strOpt(msg, "scope"), strOpt(msg, "target"), session_.samplerEntriesInUse());
        if (!r.error.empty()) throw PatchError(r.error, r.message);
        reply({{"type", "samples_delete_ok"}, {"freedBytes", r.freedBytes}});
        return;
    }
    throw PatchError("unknown_type", "unknown message type '" + type + "'");
}

std::vector<AddressedOutgoing> CloudServices::poll() {
    std::vector<AddressedOutgoing> q;
    std::vector<Continuation> run;
    {
        std::lock_guard<std::mutex> lk(laterM_);
        run.swap(later_);
    }
    for (auto& c : run) c(q);

    bool jobsEnded = false;
    for (auto& e : samples_->poll(jobsEnded)) q.push_back({{}, {Outgoing::Target::Broadcast, std::move(e)}});
    if (jobsEnded) session_.notifyResourcesChanged();
    // A sampler waits for samples while the server is unreachable: retry the catalog (then its download).
    if (samples_->wantsManifestRetry()) client_->post([this] { samples_->refreshManifest(10000); });

    if (const uint64_t v = client_->statusVersion(); v != lastCloudVersion_) {
        lastCloudVersion_ = v;
        relay_->wake();                       // link / token may have changed
        const bool switched = applyProfile(); // token dropped after a 401
        q.push_back({{}, {Outgoing::Target::Broadcast, statusEvent(nullptr)}});
        if (switched) q.push_back({{}, {Outgoing::Target::Broadcast, session_.stateJson()}});
        if (switched) q.push_back({{}, {Outgoing::Target::Broadcast, session_.songStateJson()}});
    }
    const auto now = std::chrono::steady_clock::now();
    if (const uint64_t v = sync_->statusVersion(); v != lastSyncVersion_ && now - lastSyncEmit_ >= std::chrono::milliseconds(500)) {
        lastSyncVersion_ = v;
        lastSyncEmit_ = now;
        json s = sync_->statusJson();
        s["type"] = "sync_status";
        q.push_back({{}, {Outgoing::Target::Broadcast, std::move(s)}});
    }
    return q;
}

} // namespace ks::cloud
