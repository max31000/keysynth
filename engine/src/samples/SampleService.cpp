#include "samples/SampleService.h"

#include "cloud/CloudClient.h"
#include "core/AppPaths.h"
#include "core/Sha256.h"
#include "samples/SamplePathRules.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace ks::samples {

namespace {

json nullable(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

bool writeFileAtomic(const fs::path& file, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!o) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

// Remove empty directories from `dir` up to (excluding) `stop`.
void pruneEmptyDirs(fs::path dir, const fs::path& stop) {
    std::error_code ec;
    while (!dir.empty() && isPathInsideLexical(dir, stop) && dir.lexically_normal() != stop.lexically_normal()) {
        if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) break;
        fs::remove(dir, ec);
        dir = dir.parent_path();
    }
}

} // namespace

SampleService::SampleService(cloud::CloudClient& client, Options opt) : client_(client), opt_(std::move(opt)) {}

SampleService::~SampleService() { stop(); }

void SampleService::start() {
    {
        std::lock_guard<std::mutex> lk(m_);
        index_.load(opt_.cacheRoot);
        std::ifstream f(opt_.cacheRoot / "manifest.json", std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            if (auto m = Manifest::parse(ss.str())) manifest_ = std::make_shared<const Manifest>(std::move(*m));
        }
        if (!manifest_) index_.manifestEtag.clear();
        stop_ = false;
    }
    for (int i = 0; i < std::max(1, opt_.workers); ++i) workers_.emplace_back([this] { workerLoop(); });
}

void SampleService::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        for (auto& [k, t] : tasks_) t.cancel->store(true);
    }
    cv_.notify_all();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
    workers_.clear();
}

bool SampleService::idle() const {
    std::lock_guard<std::mutex> lk(m_);
    return jobs_.empty();
}

bool SampleService::hasManifest() const {
    std::lock_guard<std::mutex> lk(m_);
    return manifest_ != nullptr;
}

std::string SampleService::manifestState() const {
    std::lock_guard<std::mutex> lk(m_);
    return !manifest_ ? "none" : manifestFresh_ ? "ok" : "stale";
}

// --------------------------------------------------------------------------------------------------------------
// Manifest

std::string SampleService::refreshManifest(int timeoutMs) {
    std::string etag;
    {
        std::lock_guard<std::mutex> lk(m_);
        lastManifestCheck_ = Clock::now();
        etag = manifest_ ? index_.manifestEtag : std::string();
    }
    if (!client_.linked()) return manifestState() == "none" ? "none" : "stale";
    cloud::HttpRequest req;
    req.url = "/api/samples/manifest";
    req.timeoutMs = timeoutMs;
    if (!etag.empty()) req.headers.emplace_back("If-None-Match", etag);
    const cloud::HttpResponse r = client_.api(req);
    std::lock_guard<std::mutex> lk(m_);
    if (r.status == 304 && manifest_) {
        manifestFresh_ = true;
        index_.manifestFetchedAt = nowIso8601();
        index_.save(opt_.cacheRoot);
    } else if (r.status == 200) {
        if (auto m = Manifest::parse(r.body)) {
            if (writeFileAtomic(opt_.cacheRoot / "manifest.json", r.body)) {
                manifest_ = std::make_shared<const Manifest>(std::move(*m));
                manifestFresh_ = true;
                index_.manifestEtag = r.header("etag");
                index_.manifestFetchedAt = nowIso8601();
                index_.save(opt_.cacheRoot);
            }
        }
    }
    return !manifest_ ? "none" : manifestFresh_ ? "ok" : "stale";
}

void SampleService::maybeRefreshManifest() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (lastManifestCheck_ != Clock::time_point{} && Clock::now() - lastManifestCheck_ < std::chrono::minutes(1)) return;
    }
    refreshManifest(10000);
}

// --------------------------------------------------------------------------------------------------------------
// Listing

bool SampleService::isLocal(const std::string& lib, const std::string& sfz) const {
    std::error_code ec;
    for (const auto& d : opt_.localAssetDirs) {
        const fs::path p = (d / "samples" / pathFromUtf8(lib) / pathFromUtf8(sfz)).lexically_normal();
        if (isPathInsideLexical(p, d) && fs::is_regular_file(p, ec)) return true;
    }
    return false;
}

std::string SampleService::coveringJob(const std::string& lib, const std::string& entryId) const {
    for (const char* id : {"entry:", "library:"}) {
        const std::string jid = std::string(id) + (id[0] == 'e' ? entryId : lib);
        if (jobs_.count(jid)) return jid;
    }
    if (jobs_.count("all")) return "all";
    return {};
}

json SampleService::listJson(const std::string& state) const {
    std::lock_guard<std::mutex> lk(m_);
    json out;
    out["manifest"] = {{"state", state},
                       {"etag", manifest_ ? nullable(index_.manifestEtag) : json(nullptr)},
                       {"fetchedAt", manifest_ ? nullable(index_.manifestFetchedAt) : json(nullptr)}};
    // From the index (no directory walk on the message thread): verified files + resumable partials.
    uint64_t cacheBytes = 0;
    for (const auto& [k, c] : index_.files) cacheBytes += c.bytes;
    for (const auto& [k, p] : index_.partial) cacheBytes += p.bytesDone;
    out["cacheBytes"] = cacheBytes;
    std::error_code ec;
    fs::create_directories(opt_.cacheRoot, ec);
    const auto sp = fs::space(opt_.cacheRoot, ec);
    out["freeBytes"] = ec ? uint64_t{0} : static_cast<uint64_t>(sp.available);
    json groups = json::array();
    if (manifest_) {
        for (const auto& g : manifest_->groups) {
            json libs = json::array();
            for (const auto& lib : manifest_->libraries) {
                if (lib.group != g.id) continue;
                json entries = json::array();
                bool anyLocal = false, allCached = !lib.entries.empty();
                for (const auto& e : lib.entries) {
                    uint64_t cached = 0;
                    EntryStatus st = index_.entryStatus(lib, e, &cached);
                    if (isLocal(lib.id, e.sfz)) st = EntryStatus::Local;
                    anyLocal = anyLocal || st == EntryStatus::Local;
                    allCached = allCached && st == EntryStatus::Cached;
                    const std::string job = coveringJob(lib.id, e.id);
                    entries.push_back({{"id", e.id},
                                       {"name", e.name},
                                       {"sfz", "assets/samples/" + e.id},
                                       {"bytes", e.bytes},
                                       {"cachedBytes", cached},
                                       {"status", toString(st)},
                                       {"job", nullable(job)}});
                }
                uint64_t libCached = 0;
                for (const auto& f : lib.files)
                    if (index_.isCached(lib.id, f)) libCached += f.bytes;
                const char* status = anyLocal ? "local" : allCached ? "cached" : libCached == 0 ? "missing" : "partial";
                std::string job = jobs_.count("library:" + lib.id) ? "library:" + lib.id : jobs_.count("all") ? "all" : "";
                libs.push_back({{"id", lib.id},
                                {"name", lib.name},
                                {"licence", lib.licence},
                                {"author", lib.author},
                                {"attribution", lib.attribution},
                                {"bytes", lib.bytes},
                                {"cachedBytes", libCached},
                                {"status", status},
                                {"job", nullable(job)},
                                {"entries", entries}});
            }
            groups.push_back({{"id", g.id}, {"name", g.name}, {"libraries", libs}});
        }
    }
    out["groups"] = groups;
    return out;
}

// --------------------------------------------------------------------------------------------------------------
// Jobs

std::vector<std::string> SampleService::targetEntries(const std::string& scope, const std::string& target) const {
    std::vector<std::string> out;
    if (!manifest_) return out;
    if (scope == "entry") {
        if (manifest_->entry(target)) out.push_back(target);
    } else if (scope == "library") {
        if (const auto* l = manifest_->library(target))
            for (const auto& e : l->entries) out.push_back(e.id);
    } else if (scope == "all") {
        for (const auto& l : manifest_->libraries)
            for (const auto& e : l.entries) out.push_back(e.id);
    }
    return out;
}

void SampleService::startJobLocked(Job j, bool front) {
    // Files to download: unique files of non-local entries that are not cached yet.
    std::set<std::string> keys;
    for (const auto& eid : j.entries) {
        const auto le = manifest_->entry(eid);
        if (!le) continue;
        const ManifestLibrary& lib = *le->first;
        if (isLocal(lib.id, le->second->sfz)) continue;
        for (size_t i : le->second->files) {
            const ManifestFile& f = lib.files[i];
            if (index_.isCached(lib.id, f)) continue;
            const std::string key = fileKey(lib.id, f.path);
            if (!keys.insert(key).second) continue;
            j.files.push_back(key);
            j.bytesTotal += f.bytes;
            auto [it, inserted] = tasks_.try_emplace(key);
            FileTask& t = it->second;
            if (inserted) {
                t.key = key;
                t.lib = lib.id;
                t.path = f.path;
                t.bytes = f.bytes;
                t.sha256 = f.sha256;
                t.valid = f.valid;
            }
            t.jobs.insert(j.id);
        }
    }
    // .sfz files last: an SFZ in the cache means its samples are there too (the sampler resolves by the .sfz).
    std::stable_partition(j.files.begin(), j.files.end(), [](const std::string& k) {
        return !(k.size() > 4 && (k.compare(k.size() - 4, 4, ".sfz") == 0 || k.compare(k.size() - 4, 4, ".SFZ") == 0));
    });
    j.pending = keys;
    j.filesTotal = j.files.size();
    const std::string id = j.id;
    jobs_[id] = std::move(j);
    if (front) order_.push_front(id);
    else order_.push_back(id);
    hadJobs_ = true;
    if (jobs_[id].pending.empty()) endJob(id, false, "", "", "");
    else cv_.notify_all();
}

SampleService::Result SampleService::fetch(const std::string& scope, const std::string& target) {
    Result r;
    if (scope != "entry" && scope != "library" && scope != "all") {
        r.error = "bad_request";
        r.message = "scope must be entry, library or all";
        return r;
    }
    if (scope != "all" && target.empty()) {
        r.error = "bad_request";
        r.message = "missing field 'target'";
        return r;
    }
    if (!client_.linked()) {
        r.error = "not_linked";
        r.message = "link this engine to your account first";
        return r;
    }
    std::lock_guard<std::mutex> lk(m_);
    const std::string id = scope == "all" ? "all" : scope + ":" + target;
    if (jobs_.count(id)) {
        r.job = id;
        r.existing = true;
        return r;
    }
    if (!manifest_) {
        r.error = "not_found";
        r.message = "no sample manifest yet";
        return r;
    }
    Job j;
    j.id = id;
    j.scope = scope;
    j.target = scope == "all" ? "" : target;
    j.entries = targetEntries(scope, target);
    if (j.entries.empty() && scope != "all") {
        r.error = "not_found";
        r.message = "unknown " + scope + " '" + target + "'";
        return r;
    }
    r.job = id;
    startJobLocked(std::move(j), false);
    return r;
}

SampleService::Result SampleService::cancel(const std::string& job) {
    Result r;
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> ids;
    if (job.empty()) ids.assign(order_.begin(), order_.end());
    else if (jobs_.count(job)) ids.push_back(job);
    for (const auto& id : ids) endJob(id, true, "", "", "");
    r.jobs = ids;
    return r;
}

SampleService::Result SampleService::remove(const std::string& scope, const std::string& target,
                                            const std::set<std::string>& inUse) {
    Result r;
    if (scope != "entry" && scope != "library" && scope != "all") {
        r.error = "bad_request";
        r.message = "scope must be entry, library or all";
        return r;
    }
    std::lock_guard<std::mutex> lk(m_);
    const std::vector<std::string> entries = targetEntries(scope, target);
    if (entries.empty() && scope != "all") {
        r.error = "not_found";
        r.message = manifest_ ? "unknown " + scope + " '" + target + "'" : "no sample manifest";
        return r;
    }
    const std::set<std::string> targetSet(entries.begin(), entries.end());
    for (const auto& e : entries)
        if (inUse.count(e)) {
            r.error = "in_use";
            r.message = "'" + e + "' is used by the current patch: unload it first";
            return r;
        }
    size_t localCount = 0;
    for (const auto& e : entries) {
        const auto le = manifest_->entry(e);
        if (le && isLocal(le->first->id, le->second->sfz)) ++localCount;
    }
    if (!entries.empty() && localCount == entries.size()) {
        r.error = "read_only";
        r.message = "local sample library (repo assets / KS_ASSETS_DIR): not managed by the cache";
        return r;
    }
    // Cancel jobs covering the target first.
    std::vector<std::string> cancelIds;
    for (const auto& id : order_) {
        const Job& j = jobs_[id];
        if (std::any_of(j.entries.begin(), j.entries.end(), [&](const std::string& e) { return targetSet.count(e) > 0; }))
            cancelIds.push_back(id);
    }
    for (const auto& id : cancelIds) endJob(id, true, "", "", "");

    // Files still referenced by another (cached or partial) entry stay.
    std::set<std::string> keep, drop;
    for (const auto& lib : manifest_->libraries)
        for (const auto& e : lib.entries) {
            const bool isTarget = targetSet.count(e.id) > 0 && !isLocal(lib.id, e.sfz);
            if (!isTarget && index_.entryStatus(lib, e) == EntryStatus::Missing) continue;
            for (size_t i : e.files) (isTarget ? drop : keep).insert(fileKey(lib.id, lib.files[i].path));
        }
    std::error_code ec;
    for (const auto& key : drop) {
        if (keep.count(key)) continue;
        const auto split = splitEntryId(key);
        if (!split) continue;
        const auto p = cachePathFor(opt_.cacheRoot, split->first, split->second);
        if (!p) continue;
        bool removed = false;
        if (auto it = index_.files.find(key); it != index_.files.end()) {
            const uint64_t size = fs::file_size(*p, ec);
            if (fs::remove(*p, ec) && !ec) {
                r.freedBytes += size;
                removed = true;
            }
            if (removed || !fs::exists(*p, ec)) index_.files.erase(it);
        }
        fs::path part = *p;
        part += ".part";
        const uint64_t partSize = fs::file_size(part, ec);
        if (!ec && fs::remove(part, ec)) r.freedBytes += partSize;
        index_.partial.erase(key);
        pruneEmptyDirs(p->parent_path(), opt_.cacheRoot / pathFromUtf8(split->first));
    }
    if (scope == "all" || scope == "library") {
        // Library dirs left empty.
        for (const auto& lib : manifest_->libraries)
            if (scope == "all" || lib.id == target) {
                const fs::path d = opt_.cacheRoot / pathFromUtf8(lib.id);
                if (fs::is_directory(d, ec) && fs::is_empty(d, ec)) fs::remove(d, ec);
            }
    }
    index_.save(opt_.cacheRoot);
    return r;
}

SampleFetchState SampleService::autoFetch(const std::string& entryId, bool fresh) {
    const auto split = splitEntryId(entryId);
    if (!split) return SampleFetchState::Unavailable;
    std::lock_guard<std::mutex> lk(m_);
    if (!coveringJob(split->first, entryId).empty()) return SampleFetchState::Waiting;
    if (fresh) {
        autoFailed_.erase(entryId);
        autoCancelled_.erase(entryId);
    } else {
        if (autoFailed_.count(entryId)) return SampleFetchState::Failed;
        if (autoCancelled_.count(entryId)) return SampleFetchState::Unavailable;
    }
    if (!manifest_ || !manifest_->entry(entryId) || !client_.linked()) return SampleFetchState::Unavailable;
    if (isLocal(split->first, split->second)) return SampleFetchState::Unavailable;
    if (const auto le = manifest_->entry(entryId); index_.entryStatus(*le->first, *le->second) == EntryStatus::Cached)
        return SampleFetchState::Unavailable; // complete: nothing to wait for
    Job j;
    j.id = "entry:" + entryId;
    j.scope = "entry";
    j.target = entryId;
    j.autoFetch = true;
    j.entries = {entryId};
    startJobLocked(std::move(j), true);
    return jobs_.count("entry:" + entryId) ? SampleFetchState::Waiting : SampleFetchState::Unavailable;
}

void SampleService::endJob(const std::string& jobId, bool cancelled, const std::string& code, const std::string& message,
                           const std::string& path) {
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return;
    Job j = std::move(it->second);
    jobs_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), jobId), order_.end());
    for (const auto& key : j.pending) {
        auto t = tasks_.find(key);
        if (t == tasks_.end()) continue;
        t->second.jobs.erase(jobId);
        if (t->second.jobs.empty()) {
            if (t->second.running) t->second.cancel->store(true); // the worker drops it when it returns
            else tasks_.erase(t);
        }
    }
    if (!code.empty()) {
        if (j.autoFetch)
            for (const auto& e : j.entries) autoFailed_.insert(e);
        events_.push_back({{"type", "samples_error"},
                           {"job", j.id},
                           {"scope", j.scope},
                           {"target", j.target},
                           {"code", code},
                           {"message", message},
                           {"path", nullable(path)},
                           {"notify", true}});
    } else {
        if (cancelled && j.autoFetch)
            for (const auto& e : j.entries) autoCancelled_.insert(e);
        json done = json::array();
        for (const auto& eid : j.entries)
            if (const auto le = manifest_ ? manifest_->entry(eid) : std::nullopt)
                if (index_.entryStatus(*le->first, *le->second) == EntryStatus::Cached) done.push_back(eid);
        events_.push_back({{"type", "samples_done"},
                           {"job", j.id},
                           {"scope", j.scope},
                           {"target", j.target},
                           {"cancelled", cancelled},
                           {"entries", done}});
    }
    jobsEnded_ = true;
}

uint64_t SampleService::jobBytesDone(const Job& j) const {
    uint64_t b = j.bytesFinished;
    for (const auto& key : j.pending)
        if (auto t = tasks_.find(key); t != tasks_.end() && t->second.running) b += t->second.done->load();
    return std::min(b, j.bytesTotal);
}

json SampleService::jobJson(const Job& j) {
    bool running = false;
    for (const auto& key : j.pending)
        if (auto t = tasks_.find(key); t != tasks_.end() && t->second.running) running = true;
    return {{"id", j.id},
            {"scope", j.scope},
            {"target", j.target},
            {"state", running ? "running" : "queued"},
            {"bytesDone", jobBytesDone(j)},
            {"bytesTotal", j.bytesTotal},
            {"filesDone", j.filesDone},
            {"filesTotal", j.filesTotal}};
}

std::vector<json> SampleService::poll(bool& jobsEnded) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<json> out;
    const auto now = Clock::now();
    if ((!jobs_.empty() || hadJobs_) && now - lastProgress_ >= std::chrono::milliseconds(200)) {
        lastProgress_ = now;
        json arr = json::array();
        for (const auto& id : order_) {
            Job& j = jobs_[id];
            json jj = jobJson(j);
            const uint64_t bytes = jj["bytesDone"].get<uint64_t>();
            j.rateWindow.emplace_back(now, bytes);
            while (j.rateWindow.size() > 1 && now - j.rateWindow.front().first > std::chrono::seconds(2))
                j.rateWindow.pop_front();
            double rate = 0.0;
            if (jj["state"] == "running" && j.rateWindow.size() > 1) {
                const double dt = std::chrono::duration<double>(now - j.rateWindow.front().first).count();
                if (dt > 0) rate = static_cast<double>(bytes - j.rateWindow.front().second) / dt;
            }
            jj["rate"] = std::round(rate);
            arr.push_back(std::move(jj));
        }
        out.push_back({{"type", "samples_progress"}, {"jobs", arr}});
        if (jobs_.empty()) hadJobs_ = false; // one final empty progress after the last job
    }
    for (auto& e : events_) out.push_back(std::move(e));
    events_.clear();
    jobsEnded = jobsEnded_;
    jobsEnded_ = false;
    return out;
}

// --------------------------------------------------------------------------------------------------------------
// Download workers

void SampleService::workerLoop() {
    for (;;) {
        FileTask copy;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<std::atomic<uint64_t>> done;
        {
            std::unique_lock<std::mutex> lk(m_);
            FileTask* next = nullptr;
            cv_.wait(lk, [&] {
                if (stop_) return true;
                for (const auto& id : order_) {
                    const Job& j = jobs_[id];
                    for (const auto& key : j.files) {
                        if (!j.pending.count(key)) continue;
                        auto t = tasks_.find(key);
                        if (t != tasks_.end() && !t->second.running) {
                            next = &t->second;
                            return true;
                        }
                    }
                }
                return false;
            });
            if (stop_) return;
            next->running = true;
            next->cancel->store(false);
            next->done->store(0);
            copy = *next;
            cancel = next->cancel;
            done = next->done;
        }
        const DownloadResult r = download(copy, cancel, done);
        std::lock_guard<std::mutex> lk(m_);
        finishFile(copy.key, r);
        cv_.notify_all();
    }
}

bool SampleService::sleepUnlessCancelled(int ms, const std::atomic<bool>& cancel) {
    for (int waited = 0; waited < ms; waited += 10) {
        if (cancel.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10, ms - waited)));
    }
    return !cancel.load();
}

SampleService::DownloadResult SampleService::download(const FileTask& t, const std::shared_ptr<std::atomic<bool>>& cancel,
                                                      const std::shared_ptr<std::atomic<uint64_t>>& done) {
    DownloadResult res;
    const std::string label = t.key;
    if (!t.valid) {
        res.code = "bad_path";
        res.message = "manifest path failed validation: " + label;
        return res;
    }
    const auto dest = cachePathFor(opt_.cacheRoot, t.lib, t.path);
    if (!dest) {
        res.code = "bad_path";
        res.message = "manifest path failed validation: " + label;
        return res;
    }
    std::error_code ec;
    fs::create_directories(dest->parent_path(), ec);
    if (ec) {
        res.code = "io_error";
        res.message = "cannot create " + pathToUtf8(dest->parent_path()) + ": " + ec.message();
        return res;
    }
    fs::path part = *dest;
    part += ".part";
    std::string etag;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (auto p = index_.partial.find(t.key); p != index_.partial.end()) etag = p->second.etag;
    }
    const std::string url = "/api/samples/files/" + cloud::encodePath(t.lib) + "/" + cloud::encodePath(t.path);

    int attempt = 0;
    std::string lastCode, lastMessage;
    while (attempt <= opt_.maxRetries) {
        if (cancel->load()) {
            res.outcome = Outcome::Cancelled;
            return res;
        }
        if (attempt > 0 && !sleepUnlessCancelled(opt_.retryBaseMs << (attempt - 1), *cancel)) {
            res.outcome = Outcome::Cancelled;
            return res;
        }
        ++attempt;
        uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
        if (ec) have = 0;
        if (have > t.bytes || (have > 0 && etag.empty())) {
            fs::remove(part, ec); // cannot resume safely
            have = 0;
        }
        cloud::HttpRequest req;
        req.url = url;
        req.timeoutMs = opt_.fileTimeoutMs;
        if (have > 0) {
            req.headers.emplace_back("Range", "bytes=" + std::to_string(have) + "-");
            req.headers.emplace_back("If-Range", etag);
        }
        std::ofstream out;
        bool diskFull = false, writeFailed = false, rangeMismatch = false, opened = false;
        uint64_t written = 0;
        cloud::BodySink sink = [&](const cloud::HttpResponse& head, const char* data, size_t n) {
            if (!opened) {
                opened = true;
                const bool resume = head.status == 206 && have > 0;
                if (head.status == 206) {
                    // Content-Range must start where our .part ends.
                    const std::string cr = head.header("content-range");
                    if (cr.rfind("bytes " + std::to_string(have) + "-", 0) != 0) {
                        rangeMismatch = true;
                        return false;
                    }
                }
                written = resume ? have : 0;
                const std::string newEtag = head.header("etag");
                if (!resume || etag.empty()) etag = newEtag;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    index_.partial[t.key] = {written, etag};
                    index_.save(opt_.cacheRoot);
                }
                out.open(part, std::ios::binary | (resume ? std::ios::app : std::ios::trunc));
                if (!out) {
                    writeFailed = true;
                    return false;
                }
            }
            out.write(data, static_cast<std::streamsize>(n));
            if (!out) {
                std::error_code sec;
                const auto sp = fs::space(part.parent_path(), sec);
                diskFull = !sec && sp.available < n + 4096;
                writeFailed = true;
                return false;
            }
            written += n;
            done->store(written);
            return written <= t.bytes; // a server sending more than the manifest says is wrong
        };
        const cloud::HttpResponse r = client_.api(req, &sink, cancel.get());
        if (out.is_open()) out.close();
        {
            std::lock_guard<std::mutex> lk(m_);
            if (opened) {
                const uint64_t sz = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
                index_.partial[t.key] = {sz, etag};
            }
        }
        if (r.cancelled || cancel->load()) {
            res.outcome = Outcome::Cancelled;
            return res;
        }
        if (diskFull) {
            res.code = "disk_full";
            res.message = "disk full while writing " + label;
            return res;
        }
        if (writeFailed) {
            res.code = "io_error";
            res.message = "cannot write " + pathToUtf8(part);
            return res;
        }
        if (rangeMismatch) {
            fs::remove(part, ec);
            etag.clear();
            lastCode = "http_error";
            lastMessage = "unexpected Content-Range for " + label;
            continue;
        }
        if (r.error == "not_linked") {
            res.code = "not_linked";
            res.message = "not linked";
            return res;
        }
        if (r.status == 0) {
            lastCode = "offline";
            lastMessage = "network error downloading " + label + (r.error.empty() ? "" : ": " + r.error);
            continue;
        }
        if (r.status == 401 || r.status == 403) {
            res.code = "unauthorized";
            res.message = "the server refused the device token (HTTP " + std::to_string(r.status) + ")";
            return res;
        }
        if (r.status == 404) {
            res.code = "not_found";
            res.message = "not on the server: " + label;
            return res;
        }
        if (r.status == 416) {
            fs::remove(part, ec);
            etag.clear();
            lastCode = "http_error";
            lastMessage = "HTTP 416 for " + label;
            continue;
        }
        if (r.status != 200 && r.status != 206) {
            lastCode = "http_error";
            lastMessage = "HTTP " + std::to_string(r.status) + " for " + label;
            continue;
        }
        if (r.sinkFailed) { // more bytes than announced
            fs::remove(part, ec);
            etag.clear();
            lastCode = "checksum_mismatch";
            lastMessage = "size mismatch for " + label;
            continue;
        }
        if (!opened) { // empty body
            out.open(part, std::ios::binary | std::ios::trunc);
            out.close();
        }
        const uint64_t size = fs::file_size(part, ec);
        if (ec || size < t.bytes) {
            lastCode = "offline";
            lastMessage = "transfer of " + label + " ended early";
            continue; // resume from what we have
        }
        const auto sha = Sha256::hexOfFile(part);
        if (!sha || *sha != t.sha256 || size != t.bytes) {
            fs::remove(part, ec);
            etag.clear();
            {
                std::lock_guard<std::mutex> lk(m_);
                index_.partial.erase(t.key);
            }
            lastCode = "checksum_mismatch";
            lastMessage = "SHA-256 mismatch for " + label;
            continue;
        }
        res.outcome = Outcome::Ok;
        return res;
    }
    res.code = lastCode.empty() ? "http_error" : lastCode;
    res.message = lastMessage;
    return res;
}

void SampleService::finishFile(const std::string& key, const DownloadResult& r) {
    auto it = tasks_.find(key);
    if (it == tasks_.end()) return;
    FileTask t = std::move(it->second);
    tasks_.erase(it);
    std::error_code ec;
    const auto dest = cachePathFor(opt_.cacheRoot, t.lib, t.path);
    fs::path part = dest ? *dest : fs::path();
    part += ".part";
    if (t.jobs.empty()) {
        // Nobody wants it any more (cancelled / deleted meanwhile): keep a .part for resume, drop a finished one.
        if (r.outcome == Outcome::Ok && dest) {
            fs::remove(part, ec);
            index_.partial.erase(key);
        }
        index_.save(opt_.cacheRoot);
        return;
    }
    DownloadResult res = r;
    if (res.outcome == Outcome::Ok && dest) {
        fs::rename(part, *dest, ec);
        if (ec) {
            res.outcome = Outcome::Failed;
            res.code = "io_error";
            res.message = "cannot move " + pathToUtf8(part) + " into place (file in use?): " + ec.message();
        } else {
            index_.files[key] = {t.bytes, t.sha256, nowIso8601()};
            index_.partial.erase(key);
        }
    }
    index_.save(opt_.cacheRoot);
    if (res.outcome == Outcome::Cancelled) {
        // Cancelled while still wanted (stop()): stays queued for the next start; the jobs keep it pending.
        t.running = false;
        tasks_[key] = std::move(t);
        return;
    }
    const std::set<std::string> jobsOf = t.jobs;
    for (const auto& jid : jobsOf) {
        auto j = jobs_.find(jid);
        if (j == jobs_.end()) continue;
        if (res.outcome == Outcome::Ok) {
            j->second.pending.erase(key);
            j->second.filesDone++;
            j->second.bytesFinished += t.bytes;
            if (j->second.pending.empty()) endJob(jid, false, "", "", "");
        } else {
            j->second.pending.erase(key);
            endJob(jid, false, res.code, res.message, key);
        }
    }
}

} // namespace ks::samples
