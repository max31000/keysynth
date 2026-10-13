#pragma once
// In-memory fake of the keysynth API (docs/specs/2026-10-12-cloud-api.md subset) behind cloud::HttpTransport:
// devices/me, presets (revision + If-Match / If-None-Match, tombstones), manifest (ETag), files (Range/If-Range).

#include "cloud/HttpTransport.h"
#include "core/Sha256.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ks::test {

inline const std::string kFakeBase = "https://fake.test/keysynth";
inline const std::string kFakeToken = "ksd_" + std::string(43, 'A');
inline const std::string kOtherToken = "ksd_" + std::string(43, 'B');

struct FakeCloud final : cloud::HttpTransport {
    struct Preset {
        int64_t revision = 0;
        bool deleted = false;
        nlohmann::json patch;
    };
    struct Collection {
        std::string base, key;
        std::map<std::string, Preset>& docs;
    };
    std::mutex m;
    std::map<std::string, std::string> tokens = {{kFakeToken, "17"}, {kOtherToken, "42"}}; // token -> accountId
    std::map<std::string, std::string> accountNames = {{"17", "Max"}, {"42", "Other"}};
    bool offline = false;
    std::map<std::string, Preset> presets;
    std::map<std::string, Preset> songs; // /api/songs (body key "song")
    Collection presetsCol{"/api/presets", "patch", presets};
    Collection songsCol{"/api/songs", "song", songs};
    std::string manifest;
    std::map<std::string, std::string> files; // "<lib>/<path>" -> bytes
    size_t dropAfterBytes = 0;                // > 0: the next file transfer stops after that many bytes (status 0)
    int fileFailures = 0;                     // next N file requests answer 500
    std::vector<std::string> requests;        // "METHOD /path [Range]"
    std::atomic<int> fileRequests{0};

    static std::string urlDecode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }
    static std::string header(const cloud::HttpRequest& r, const std::string& name) {
        for (const auto& [k, v] : r.headers)
            if (k == name) return v;
        return {};
    }
    static std::string fileEtag(const std::string& bytes) { return "\"" + Sha256::hex(bytes).substr(0, 12) + "\""; }
    static nlohmann::json meta(const std::string& slug, const Preset& p) {
        return {{"slug", slug}, {"revision", p.revision}, {"updatedAt", "2026-10-12T14:00:00Z"}, {"deleted", p.deleted}};
    }
    std::optional<cloud::HttpResponse> collection(Collection& c, const std::string& path, const cloud::HttpRequest& req) {
        if (path == c.base && req.method == "GET") {
            nlohmann::json items = nlohmann::json::array();
            for (const auto& [slug, p] : c.docs) items.push_back(meta(slug, p));
            return json(200, {{"items", items}});
        }
        if (path.rfind(c.base + "/", 0) == 0) {
            const std::string slug = path.substr(c.base.size() + 1);
            auto it = c.docs.find(slug);
            if (req.method == "GET") {
                if (it == c.docs.end()) return json(404, {{"error", "not_found"}});
                nlohmann::json j = meta(slug, it->second);
                j[c.key] = it->second.deleted ? nlohmann::json(nullptr) : it->second.patch;
                return json(200, j);
            }
            const std::string ifMatch = header(req, "If-Match"), ifNone = header(req, "If-None-Match");
            auto mismatch = [&] {
                return json(412, {{"error", "revision_mismatch"},
                                  {"current", it == c.docs.end() ? nlohmann::json(nullptr) : meta(slug, it->second)}});
            };
            if (req.method == "PUT") {
                const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
                if (!body.is_object() || !body.contains(c.key)) return json(400, {{"error", "invalid_" + c.key}});
                if (ifNone == "*") {
                    if (it != c.docs.end()) return mismatch();
                    c.docs[slug] = {1, false, body[c.key]};
                    return json(201, meta(slug, c.docs[slug]));
                }
                if (ifMatch.empty()) return json(428, {{"error", "precondition_required"}});
                if (it == c.docs.end() || ifMatch != "\"" + std::to_string(it->second.revision) + "\"") return mismatch();
                it->second = {it->second.revision + 1, false, body[c.key]};
                return json(200, meta(slug, it->second));
            }
            if (req.method == "DELETE") {
                if (it == c.docs.end()) return json(404, {{"error", "not_found"}});
                if (ifMatch != "\"" + std::to_string(it->second.revision) + "\"") return mismatch();
                it->second = {it->second.revision + 1, true, nullptr};
                return json(200, meta(slug, it->second));
            }
        }
        return std::nullopt;
    }
    static cloud::HttpResponse json(int status, const nlohmann::json& j) {
        cloud::HttpResponse r;
        r.status = status;
        r.body = j.dump();
        r.headers["content-type"] = "application/json";
        return r;
    }

    cloud::HttpResponse send(const cloud::HttpRequest& req, const cloud::BodySink* sink,
                             const std::atomic<bool>* cancel) override {
        std::unique_lock<std::mutex> lk(m);
        cloud::HttpResponse r;
        if (offline) {
            r.error = "offline";
            return r;
        }
        if (req.url.rfind(kFakeBase, 0) != 0) return json(404, {{"error", "not_found"}});
        const std::string path = req.url.substr(kFakeBase.size());
        requests.push_back(req.method + " " + path + (header(req, "Range").empty() ? "" : " " + header(req, "Range")));
        const std::string auth = header(req, "Authorization");
        const auto tok = tokens.find(auth.rfind("Bearer ", 0) == 0 ? auth.substr(7) : std::string());
        if (tok == tokens.end()) return json(401, {{"error", "token_invalid"}});
        const std::string account = tok->second;

        if (path == "/api/devices/me") {
            if (req.method == "DELETE") {
                r.status = 204;
                return r;
            }
            return json(200, {{"deviceId", "x"}, {"name", "PC"}, {"accountId", account},
                              {"accountName", accountNames[account]}, {"expiresAt", "2027-04-10T12:00:00Z"}});
        }
        if (path == "/api/samples/manifest") {
            if (manifest.empty()) return json(503, {{"error", "index_unavailable"}});
            const std::string etag = "\"" + Sha256::hex(manifest).substr(0, 16) + "\"";
            if (header(req, "If-None-Match") == etag) {
                r.status = 304;
                return r;
            }
            r.status = 200;
            r.body = manifest;
            r.headers["etag"] = etag;
            return r;
        }
        if (path.rfind("/api/samples/files/", 0) == 0) {
            ++fileRequests;
            const std::string rel = path.substr(std::string("/api/samples/files/").size());
            std::string key;
            size_t start = 0;
            while (true) { // decode segment by segment
                const size_t slash = rel.find('/', start);
                key += urlDecode(rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
                if (slash == std::string::npos) break;
                key += "/";
                start = slash + 1;
            }
            if (fileFailures > 0) {
                --fileFailures;
                return json(500, {{"error", "internal"}});
            }
            auto f = files.find(key);
            if (f == files.end()) return json(404, {{"error", "not_found"}});
            const std::string bytes = f->second;
            const std::string etag = fileEtag(bytes);
            size_t from = 0;
            r.status = 200;
            const std::string range = header(req, "Range");
            if (!range.empty() && header(req, "If-Range") == etag) {
                from = std::stoull(range.substr(6));
                if (from >= bytes.size()) return json(416, {{"error", "range"}});
                r.status = 206;
                r.headers["content-range"] =
                    "bytes " + std::to_string(from) + "-" + std::to_string(bytes.size() - 1) + "/" + std::to_string(bytes.size());
            }
            r.headers["etag"] = etag;
            size_t end = bytes.size();
            bool drop = false;
            if (dropAfterBytes > 0 && from + dropAfterBytes < end) {
                end = from + dropAfterBytes;
                dropAfterBytes = 0;
                drop = true;
            }
            lk.unlock();
            for (size_t i = from; i < end; i += 1000) {
                if (cancel && cancel->load()) {
                    cloud::HttpResponse c;
                    c.cancelled = true;
                    return c;
                }
                const size_t n = std::min<size_t>(1000, end - i);
                if (sink) {
                    if (!(*sink)(r, bytes.data() + i, n)) {
                        r.sinkFailed = true;
                        return r;
                    }
                } else {
                    r.body.append(bytes, i, n);
                }
            }
            if (drop) {
                cloud::HttpResponse d;
                d.error = "connection dropped";
                return d;
            }
            return r;
        }
        for (auto* col : {&presetsCol, &songsCol}) {
            if (auto hit = collection(*col, path, req)) return *hit;
        }
        return json(404, {{"error", "not_found"}});
    }
};

} // namespace ks::test
