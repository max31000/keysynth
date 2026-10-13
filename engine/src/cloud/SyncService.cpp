#include "cloud/SyncService.h"

#include "cloud/CloudClient.h"
#include "core/AppPaths.h"
#include "core/Sha256.h"
#include "samples/CacheIndex.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <ctime>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ks::cloud {

namespace {

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeAtomic(const fs::path& file, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".sync-tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!o) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

std::string compactUtcNow() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

int64_t revOf(const json& j) {
    auto it = j.find("revision");
    return it != j.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

bool deletedOf(const json& j) {
    auto it = j.find("deleted");
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

} // namespace

bool isValidSlug(const std::string& s) {
    static const std::regex re("^[a-z0-9]+(-[a-z0-9]+)*$");
    static const std::regex reserved("^(con|prn|aux|nul|com[1-9]|lpt[1-9])$"); // Windows device names
    return !s.empty() && s.size() <= 64 && std::regex_match(s, re) && !std::regex_match(s, reserved);
}

std::string SyncEngine::conflictName(const std::string& slug, const std::string& compactUtc) {
    return slug + ".conflict-" + compactUtc + ".json";
}

SyncEngine::SyncEngine(Api api, fs::path presetsDir, fs::path syncFile, std::mutex* fileMutex, SyncCollection collection)
    : api_(std::move(api)), col_(std::move(collection)), dir_(std::move(presetsDir)), syncFile_(std::move(syncFile)),
      fileMutex_(fileMutex) {
    loadBase();
}

void SyncEngine::loadBase() {
    base_.clear();
    const auto text = readFile(syncFile_);
    if (!text) return;
    const json j = json::parse(*text, nullptr, false);
    if (!j.is_object()) return;
    if (auto l = j.find("lastSyncAt"); l != j.end() && l->is_string()) lastSyncAt_ = l->get<std::string>();
    if (auto p = j.find(col_.baseKey); p != j.end() && p->is_object())
        for (const auto& [slug, v] : p->items()) {
            if (!isValidSlug(slug) || !v.is_object()) continue;
            SyncBase b;
            b.revision = v.value("baseRevision", int64_t{0});
            b.hash = v.value("hash", std::string());
            base_[slug] = b;
        }
}

bool SyncEngine::saveBase() {
    json p = json::object();
    for (const auto& [slug, b] : base_) p[slug] = {{"baseRevision", b.revision}, {"hash", b.hash}};
    // Read-modify-write: the other collection's section stays (callers hold the profile mutex).
    json j = json::object();
    if (const auto text = readFile(syncFile_)) {
        j = json::parse(*text, nullptr, false);
        if (!j.is_object()) j = json::object();
    }
    j["format"] = 1;
    j["lastSyncAt"] = lastSyncAt_.empty() ? (j.contains("lastSyncAt") ? j["lastSyncAt"] : json(nullptr)) : json(lastSyncAt_);
    j[col_.baseKey] = p;
    return writeAtomic(syncFile_, j.dump(2) + "\n");
}

std::string SyncEngine::lastSyncAt() const { return lastSyncAt_; }

std::map<std::string, std::string> SyncEngine::scanLocal() const {
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
        const std::string slug = pathToUtf8(e.path().stem());
        if (!isValidSlug(slug)) continue; // conflict files (<slug>.conflict-<ts>) and odd names are not synced
        if (auto h = Sha256::hexOfFile(e.path())) out[slug] = *h;
    }
    return out;
}

int SyncEngine::pendingCount() const {
    std::unique_lock<std::mutex> lk;
    if (fileMutex_) lk = std::unique_lock<std::mutex>(*fileMutex_);
    const auto local = scanLocal();
    int n = 0;
    for (const auto& [slug, h] : local) {
        auto b = base_.find(slug);
        if (b == base_.end() || b->second.hash != h) ++n;
    }
    for (const auto& [slug, b] : base_)
        if (!b.hash.empty() && !local.count(slug)) ++n;
    return n;
}

std::vector<SyncConflict> SyncEngine::conflicts(const fs::path& dir, const std::string& logicalDir) {
    static const std::regex re(R"(^([a-z0-9]+(?:-[a-z0-9]+)*)\.conflict-(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z(?:-\d+)?\.json$)");
    std::vector<SyncConflict> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = pathToUtf8(e.path().filename());
        std::smatch m;
        if (!std::regex_match(name, m, re)) continue;
        out.push_back({m[1].str(), logicalDir + name,
                       m[2].str() + "-" + m[3].str() + "-" + m[4].str() + "T" + m[5].str() + ":" + m[6].str() + ":" +
                           m[7].str() + "Z"});
    }
    std::sort(out.begin(), out.end(), [](const SyncConflict& a, const SyncConflict& b) { return a.file < b.file; });
    return out;
}

SyncEngine::Step SyncEngine::fail(const HttpResponse& r) {
    Step s;
    s.ok = false;
    if (r.status == 0) {
        s.abort = true;
        s.error = r.error == "not_linked" ? "not_linked" : "offline";
    } else if (r.status == 401) {
        s.abort = true;
        s.error = "unauthorized";
    } else if (r.status == 429) {
        s.abort = true;
        s.error = "rate_limited";
    } else if (r.status == 413) {
        s.error = "payload_too_large";
    } else if (r.status >= 500) {
        s.abort = true;
        s.error = "http_error";
    } else {
        const json j = json::parse(r.body, nullptr, false);
        s.error = j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "http_error";
    }
    return s;
}

SyncEngine::Step SyncEngine::download(const std::string& slug, SyncPassResult& res) {
    HttpRequest req;
    req.url = url(slug);
    const HttpResponse r = api_(req);
    if (r.status == 404) { // gone meanwhile: forget it
        std::lock_guard<std::mutex> lk(*fileMutex_);
        base_.erase(slug);
        saveBase();
        return {};
    }
    if (r.status != 200) return fail(r);
    const json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return {false, false, "http_error"};
    const int64_t rev = revOf(j);
    std::lock_guard<std::mutex> lk(*fileMutex_);
    std::error_code ec;
    if (deletedOf(j) || !j.contains(col_.bodyKey) || !j[col_.bodyKey].is_object()) {
        if (fs::exists(fileOf(slug), ec)) {
            fs::remove(fileOf(slug), ec);
            ++res.deleted;
        }
        base_[slug] = {rev, ""};
    } else {
        const std::string bytes = j[col_.bodyKey].dump(2) + "\n";
        if (!writeAtomic(fileOf(slug), bytes)) return {false, false, "io_error"};
        base_[slug] = {rev, Sha256::hex(bytes)};
        ++res.pulled;
    }
    saveBase();
    return {};
}

SyncEngine::Step SyncEngine::upload(const std::string& slug, int64_t ifMatch, bool create, SyncPassResult& res,
                                    ServerMeta* conflict) {
    std::string bytes;
    {
        std::lock_guard<std::mutex> lk(*fileMutex_);
        auto b = readFile(fileOf(slug));
        if (!b) return {}; // vanished meanwhile: next pass
        bytes = std::move(*b);
    }
    const json patch = json::parse(bytes, nullptr, false);
    if (!patch.is_object() || !patch.contains("format") || !patch["format"].is_number())
        return {false, false, "invalid_" + col_.bodyKey};
    HttpRequest req;
    req.method = "PUT";
    req.url = url(slug);
    req.body = json{{col_.bodyKey, patch}}.dump();
    if (create) req.headers.emplace_back("If-None-Match", "*");
    else req.headers.emplace_back("If-Match", "\"" + std::to_string(ifMatch) + "\"");
    const HttpResponse r = api_(req);
    if (r.status == 412) {
        const json j = json::parse(r.body, nullptr, false);
        if (conflict && j.is_object() && j.contains("current") && j["current"].is_object()) {
            conflict->revision = revOf(j["current"]);
            conflict->deleted = deletedOf(j["current"]);
        }
        return {false, false, "revision_mismatch"};
    }
    if (r.status != 200 && r.status != 201) return fail(r);
    const json j = json::parse(r.body, nullptr, false);
    std::lock_guard<std::mutex> lk(*fileMutex_);
    base_[slug] = {j.is_object() ? revOf(j) : ifMatch + 1, Sha256::hex(bytes)};
    saveBase();
    ++res.pushed;
    return {};
}

SyncEngine::Step SyncEngine::remove(const std::string& slug, int64_t ifMatch, SyncPassResult& res, ServerMeta* conflict) {
    HttpRequest req;
    req.method = "DELETE";
    req.url = url(slug);
    req.headers.emplace_back("If-Match", "\"" + std::to_string(ifMatch) + "\"");
    const HttpResponse r = api_(req);
    if (r.status == 412) {
        const json j = json::parse(r.body, nullptr, false);
        if (conflict && j.is_object() && j.contains("current") && j["current"].is_object()) {
            conflict->revision = revOf(j["current"]);
            conflict->deleted = deletedOf(j["current"]);
        }
        return {false, false, "revision_mismatch"};
    }
    std::lock_guard<std::mutex> lk(*fileMutex_);
    if (r.status == 404) {
        base_.erase(slug);
        saveBase();
        return {};
    }
    if (r.status != 200) return fail(r);
    const json j = json::parse(r.body, nullptr, false);
    base_[slug] = {j.is_object() ? revOf(j) : ifMatch + 1, ""};
    saveBase();
    ++res.deleted;
    return {};
}

SyncEngine::Step SyncEngine::conflictThenDownload(const std::string& slug, SyncPassResult& res) {
    {
        std::lock_guard<std::mutex> lk(*fileMutex_);
        std::error_code ec;
        if (fs::exists(fileOf(slug), ec)) {
            const std::string stamp = compactUtcNow();
            fs::path target = dir_ / conflictName(slug, stamp);
            for (int i = 2; fs::exists(target, ec) && i < 100; ++i) // two conflicts in one second
                target = dir_ / (slug + ".conflict-" + stamp + "-" + std::to_string(i) + ".json");
            fs::rename(fileOf(slug), target, ec);
            if (ec) return {false, false, "io_error"};
            ++res.conflicts;
        }
    }
    return download(slug, res);
}

SyncEngine::Step SyncEngine::resolveServerChanged(const std::string& slug, const ServerMeta& s, SyncPassResult& res) {
    if (s.deleted) { // tombstone vs local edit: edit wins
        ServerMeta again;
        Step st = upload(slug, s.revision, false, res, &again);
        if (!st.ok && st.error == "revision_mismatch") return {}; // raced again: next pass
        return st;
    }
    return conflictThenDownload(slug, res); // server wins, local kept as conflict file
}

SyncPassResult SyncEngine::runPass(const std::atomic<bool>* cancel) {
    SyncPassResult res;
    HttpRequest listReq;
    listReq.url = col_.endpoint;
    const HttpResponse lr = api_(listReq);
    if (lr.status == 404 && col_.optional) { // older API without this collection: nothing to sync
        res.ok = true;
        return res;
    }
    if (lr.status != 200) {
        res.error = fail(lr).error;
        return res;
    }
    const json lj = json::parse(lr.body, nullptr, false);
    if (!lj.is_object() || !lj.contains("items") || !lj["items"].is_array()) {
        res.error = "http_error";
        return res;
    }
    std::map<std::string, ServerMeta> server;
    for (const auto& it : lj["items"]) {
        if (!it.is_object() || !it.contains("slug") || !it["slug"].is_string()) continue;
        const std::string slug = it["slug"].get<std::string>();
        if (!isValidSlug(slug)) continue; // never trust server names as file names
        server[slug] = {revOf(it), deletedOf(it)};
    }
    std::map<std::string, std::string> local;
    {
        std::lock_guard<std::mutex> lk(*fileMutex_);
        local = scanLocal();
    }
    std::set<std::string> slugs;
    for (const auto& [s, m] : server) slugs.insert(s);
    for (const auto& [s, h] : local) slugs.insert(s);
    for (const auto& [s, b] : base_) slugs.insert(s);

    std::string softError;
    for (const auto& slug : slugs) {
        if (cancel && cancel->load()) {
            res.error = "cancelled";
            return res;
        }
        const auto si = server.find(slug);
        const auto li = local.find(slug);
        const auto bi = base_.find(slug);
        const bool hasS = si != server.end(), hasL = li != local.end(), hasB = bi != base_.end();
        const ServerMeta S = hasS ? si->second : ServerMeta{};
        const SyncBase B = hasB ? bi->second : SyncBase{};
        Step st;
        if (hasL && !hasB) {
            // Local new (or initial link).
            if (!hasS) {
                ServerMeta c;
                st = upload(slug, 0, true, res, &c);
                if (!st.ok && st.error == "revision_mismatch") st = resolveServerChanged(slug, c, res);
            } else if (S.deleted) {
                st = resolveServerChanged(slug, S, res);
            } else {
                // Both sides have it: identical content -> just record; else server wins + conflict file.
                HttpRequest req;
                req.url = url(slug);
                const HttpResponse r = api_(req);
                if (r.status != 200) {
                    st = fail(r);
                } else {
                    const json j = json::parse(r.body, nullptr, false);
                    std::optional<std::string> mine;
                    {
                        std::lock_guard<std::mutex> lk(*fileMutex_);
                        mine = readFile(fileOf(slug));
                    }
                    const json localJ = mine ? json::parse(*mine, nullptr, false) : json();
                    if (j.is_object() && j.contains(col_.bodyKey) && mine && j[col_.bodyKey] == localJ) {
                        std::lock_guard<std::mutex> lk(*fileMutex_);
                        base_[slug] = {revOf(j), Sha256::hex(*mine)};
                        saveBase();
                    } else {
                        st = conflictThenDownload(slug, res);
                    }
                }
            }
        } else if (hasL && hasB) {
            const bool changed = li->second != B.hash;
            if (!hasS) {
                ServerMeta c; // purged server row (or never uploaded): re-upload
                st = upload(slug, 0, true, res, &c);
                if (!st.ok && st.error == "revision_mismatch") st = resolveServerChanged(slug, c, res);
            } else if (!changed) {
                if (S.revision > B.revision) st = download(slug, res);
            } else if (S.revision == B.revision) {
                ServerMeta c;
                st = upload(slug, B.revision, false, res, &c);
                if (!st.ok && st.error == "revision_mismatch") st = resolveServerChanged(slug, c, res);
            } else {
                st = resolveServerChanged(slug, S, res);
            }
        } else if (!hasL && hasB) {
            if (B.hash.empty()) {
                // Known deleted locally; server revived it -> download.
                if (hasS && !S.deleted && S.revision > B.revision) st = download(slug, res);
                else if (!hasS) {
                    std::lock_guard<std::mutex> lk(*fileMutex_);
                    base_.erase(slug);
                    saveBase();
                }
            } else if (!hasS) {
                std::lock_guard<std::mutex> lk(*fileMutex_);
                base_.erase(slug);
                saveBase();
            } else if (S.revision == B.revision) {
                if (S.deleted) {
                    std::lock_guard<std::mutex> lk(*fileMutex_);
                    base_[slug] = {S.revision, ""};
                    saveBase();
                } else {
                    ServerMeta c;
                    st = remove(slug, B.revision, res, &c);
                    if (!st.ok && st.error == "revision_mismatch") st = c.deleted ? Step{} : download(slug, res);
                }
            } else {
                // Server changed while deleted locally: server edit wins.
                if (S.deleted) {
                    std::lock_guard<std::mutex> lk(*fileMutex_);
                    base_[slug] = {S.revision, ""};
                    saveBase();
                } else {
                    st = download(slug, res);
                }
            }
        } else if (hasS) {
            // Server only.
            if (S.deleted) {
                std::lock_guard<std::mutex> lk(*fileMutex_);
                base_[slug] = {S.revision, ""};
                saveBase();
            } else {
                st = download(slug, res);
            }
        }
        if (!st.ok) {
            if (st.abort) {
                res.error = st.error;
                return res;
            }
            softError = st.error;
        }
    }
    {
        std::lock_guard<std::mutex> lk(*fileMutex_);
        lastSyncAt_ = samples::nowIso8601();
        saveBase();
    }
    res.ok = softError.empty();
    res.error = softError;
    return res;
}

// --------------------------------------------------------------------------------------------------------------

SyncService::SyncService(CloudClient& client, std::mutex& fileMutex, int intervalMs)
    : client_(client), fileMutex_(fileMutex), intervalMs_(intervalMs) {}

SyncService::~SyncService() { stop(); }

void SyncService::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread([this] { loop(); });
}

void SyncService::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cancelPass_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool SyncService::enabled() const {
    std::lock_guard<std::mutex> lk(m_);
    return !dir_.empty();
}

namespace {
SyncEngine::Api noApi() {
    return [](HttpRequest) { return HttpResponse{}; };
}
std::vector<SyncConflict> allConflicts(const fs::path& presets, const fs::path& songs) {
    auto c = SyncEngine::conflicts(presets);
    if (!songs.empty()) {
        auto s = SyncEngine::conflicts(songs, SyncCollection::songs().logicalDir);
        c.insert(c.end(), s.begin(), s.end());
    }
    return c;
}
} // namespace

void SyncService::setProfile(const fs::path& presetsDir, const fs::path& syncFile, const fs::path& songsDir) {
    {
        std::lock_guard<std::mutex> lk(m_);
        dir_ = presetsDir;
        syncFile_ = syncFile;
        songsDir_ = presetsDir.empty() ? fs::path() : songsDir;
        ++profileGen_;
        requested_ = !dir_.empty();
        state_ = dir_.empty() ? "disabled" : "idle";
        error_.clear();
        lastSyncAt_.clear();
        pending_ = 0;
        conflicts_ = dir_.empty() ? std::vector<SyncConflict>{} : allConflicts(dir_, songsDir_);
        if (!dir_.empty()) {
            SyncEngine e(noApi(), dir_, syncFile_, &fileMutex_);
            lastSyncAt_ = e.lastSyncAt();
        }
    }
    cancelPass_.store(true); // a pass of the previous profile stops at its next step
    version_.fetch_add(1);
    cv_.notify_all();
}

void SyncService::requestSync() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return;
        requested_ = true;
    }
    cv_.notify_all();
}

void SyncService::notifyLocalChange() {
    fs::path dir, file, songs;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return;
        dir = dir_;
        file = syncFile_;
        songs = songsDir_;
    }
    int pending = SyncEngine(noApi(), dir, file, &fileMutex_).pendingCount();
    if (!songs.empty()) pending += SyncEngine(noApi(), songs, file, &fileMutex_, SyncCollection::songs()).pendingCount();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_ != dir) return;
        pending_ = pending;
        conflicts_ = allConflicts(dir_, songsDir_);
        requested_ = true;
    }
    version_.fetch_add(1);
    cv_.notify_all();
}

json SyncService::statusJson() const {
    std::lock_guard<std::mutex> lk(m_);
    json c = json::array();
    for (const auto& x : conflicts_) c.push_back({{"slug", x.slug}, {"file", x.file}, {"at", x.at}});
    return {{"state", state_},
            {"lastSyncAt", lastSyncAt_.empty() ? json(nullptr) : json(lastSyncAt_)},
            {"pending", pending_},
            {"conflicts", c},
            {"error", error_.empty() ? json(nullptr) : json(error_)}};
}

void SyncService::setState(const std::string& state, const std::string& error) {
    {
        std::lock_guard<std::mutex> lk(m_);
        state_ = state;
        error_ = error;
    }
    version_.fetch_add(1);
}

void SyncService::loop() {
    for (;;) {
        fs::path dir, file, songs;
        uint64_t gen = 0;
        std::string token;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(intervalMs_), [&] { return stop_ || requested_; });
            if (stop_) return;
            requested_ = false;
            if (dir_.empty()) continue;
            dir = dir_;
            file = syncFile_;
            songs = songsDir_;
            gen = profileGen_;
            state_ = "syncing";
            cancelPass_.store(false); // under m_: a setProfile() after this point cancels this pass
            token = client_.currentToken();
        }
        version_.fetch_add(1);
        // Every request of this pass uses the token of the account the profile belongs to; after a re-link the
        // client refuses it (not_linked) and the pass aborts before touching another account.
        auto api = [this, token](HttpRequest r) { return client_.apiAs(token, std::move(r)); };
        SyncEngine presets(api, dir, file, &fileMutex_);
        SyncPassResult r = presets.runPass(&cancelPass_);
        int pending = presets.pendingCount();
        std::string lastSync = presets.lastSyncAt();
        const bool aborted = !r.ok && (r.error == "offline" || r.error == "unauthorized" || r.error == "not_linked" ||
                                       r.error == "rate_limited" || r.error == "cancelled" || r.error == "http_error");
        if (!songs.empty()) {
            if (!aborted) {
                // Songs right after presets (cloud-api §3.4); constructed now so it sees the updated sync.json.
                SyncEngine songEngine(api, songs, file, &fileMutex_, SyncCollection::songs());
                const SyncPassResult sr = songEngine.runPass(&cancelPass_);
                if (r.ok && !sr.ok) r = sr;
                pending += songEngine.pendingCount();
                if (sr.ok) lastSync = songEngine.lastSyncAt();
            } else {
                pending += SyncEngine(noApi(), songs, file, &fileMutex_, SyncCollection::songs()).pendingCount();
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (gen != profileGen_) continue; // profile switched meanwhile
            pending_ = pending;
            conflicts_ = allConflicts(dir, songs);
            if (r.ok) {
                lastSyncAt_ = lastSync;
                state_ = "idle";
                error_.clear();
            } else if (r.error == "offline") {
                state_ = "offline";
                error_ = "offline";
            } else if (r.error == "unauthorized" || r.error == "not_linked") {
                state_ = "disabled";
                error_ = r.error;
            } else if (r.error != "cancelled") {
                state_ = "error";
                error_ = r.error;
            }
        }
        version_.fetch_add(1);
    }
}

} // namespace ks::cloud
