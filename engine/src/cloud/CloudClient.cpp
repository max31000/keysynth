#include "cloud/CloudClient.h"

#include "core/AppPaths.h"
#include "platform/Dpapi.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace ks::cloud {

namespace {
json nullable(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string toBase64(const std::string& bytes) {
    return juce::Base64::toBase64(bytes.data(), bytes.size()).toStdString();
}

std::optional<std::string> fromBase64(const std::string& b64) {
    juce::MemoryOutputStream out;
    if (!juce::Base64::convertFromBase64(out, juce::String(b64))) return std::nullopt;
    return std::string(static_cast<const char*>(out.getData()), out.getDataSize());
}

std::string newUuid() {
    std::string s = juce::Uuid().toDashedString().toStdString();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The `error` code of a JSON error body ({"error":"..."}), or "".
std::string errorCode(const HttpResponse& r) {
    const json j = json::parse(r.body, nullptr, false);
    return j.is_object() ? str(j, "error") : std::string();
}
} // namespace

json CloudStatus::toJson() const {
    return {{"linked", linked},
            {"accountId", linked ? nullable(accountId) : json(nullptr)},
            {"accountName", linked ? nullable(accountName) : json(nullptr)},
            {"deviceId", deviceId},
            {"expiresAt", linked ? nullable(expiresAt) : json(nullptr)},
            {"online", online},
            {"error", nullable(error)}};
}

bool isValidToken(const std::string& t) {
    static const std::regex re("^ksd_[A-Za-z0-9_-]{43}$");
    return std::regex_match(t, re);
}

bool isUuidV4(const std::string& s) {
    static const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    return std::regex_match(s, re);
}

void CloudStore::load(const fs::path& file) {
    *this = CloudStore{};
    bool dirty = false;
    std::ifstream f(file, std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        const json j = json::parse(ss.str(), nullptr, false);
        if (j.is_object()) {
            deviceId = str(j, "deviceId");
            deviceName = str(j, "deviceName");
            if (auto l = j.find("link"); l != j.end() && l->is_object()) {
                LinkInfo li{str(*l, "accountId"), str(*l, "accountName"), str(*l, "expiresAt")};
                std::optional<std::string> tok;
                if (auto blob = fromBase64(str(*l, "token"))) tok = platform::dpapiUnprotect(*blob, kTokenEntropy);
                if (tok && isValidToken(*tok) && AppPaths::isValidAccountId(li.accountId)) {
                    link = li;
                    token = *tok;
                } else {
                    dirty = true; // undecryptable / invalid: unlinked, deviceId kept
                }
            }
        }
    }
    if (!isUuidV4(deviceId)) {
        deviceId = newUuid();
        dirty = true;
    }
    if (deviceName.empty()) {
        deviceName = platform::computerName();
        if (deviceName.size() > 100) deviceName.resize(100);
        dirty = true;
    }
    if (dirty) save(file);
}

bool CloudStore::save(const fs::path& file) const {
    json j = {{"format", 1}, {"deviceId", deviceId}, {"deviceName", deviceName}, {"link", nullptr}};
    if (link && !token.empty()) {
        const auto blob = platform::dpapiProtect(token, kTokenEntropy);
        if (!blob) return false;
        j["link"] = {{"accountId", link->accountId},
                     {"accountName", link->accountName},
                     {"expiresAt", link->expiresAt},
                     {"token", toBase64(*blob)}};
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << j.dump(2) << '\n';
        if (!o) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

CloudClient::CloudClient(std::shared_ptr<HttpTransport> http, std::string baseUrl, fs::path cloudFile)
    : http_(std::move(http)), base_(std::move(baseUrl)), file_(std::move(cloudFile)) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

CloudClient::~CloudClient() { stopWorker(); }

void CloudClient::load() {
    std::lock_guard<std::mutex> lk(m_);
    store_.load(file_);
    version_.fetch_add(1);
}

void CloudClient::startWorker() {
    std::lock_guard<std::mutex> lk(qm_);
    if (worker_.joinable()) return;
    stop_ = false;
    started_ = true;
    worker_ = std::thread([this] { workerLoop(); });
}

void CloudClient::stopWorker() {
    {
        std::lock_guard<std::mutex> lk(qm_);
        stop_ = true;
        queue_.clear();
    }
    qcv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void CloudClient::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(qm_);
        if (started_ && (stop_ || !worker_.joinable())) return; // shutting down: drop
        if (!worker_.joinable()) {
            // Never started (tests without threads): run inline.
        } else {
            queue_.push_back(std::move(task));
            qcv_.notify_one();
            return;
        }
    }
    task();
}

void CloudClient::workerLoop() {
    for (;;) {
        std::function<void()> t;
        {
            std::unique_lock<std::mutex> lk(qm_);
            qcv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            t = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            t();
        } catch (...) {
        }
    }
}

CloudStatus CloudClient::status() const {
    std::lock_guard<std::mutex> lk(m_);
    CloudStatus s;
    s.linked = store_.link.has_value();
    if (store_.link) {
        s.accountId = store_.link->accountId;
        s.accountName = store_.link->accountName;
        s.expiresAt = store_.link->expiresAt;
    }
    s.deviceId = store_.deviceId;
    s.online = online_;
    s.error = error_;
    return s;
}

bool CloudClient::linked() const {
    std::lock_guard<std::mutex> lk(m_);
    return store_.link.has_value();
}

std::string CloudClient::accountId() const {
    std::lock_guard<std::mutex> lk(m_);
    return store_.link ? store_.link->accountId : std::string();
}

std::string CloudClient::deviceId() const {
    std::lock_guard<std::mutex> lk(m_);
    return store_.deviceId;
}

std::string CloudClient::currentToken() const {
    std::lock_guard<std::mutex> lk(m_);
    return store_.token;
}

void CloudClient::setOnline(bool online, const std::string& error) {
    std::lock_guard<std::mutex> lk(m_);
    if (online_ == online && error_ == error) return;
    online_ = online;
    error_ = error;
    version_.fetch_add(1);
}

HttpResponse CloudClient::apiAs(const std::string& token, HttpRequest req) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (token.empty() || store_.token != token) {
            HttpResponse r;
            r.error = "not_linked";
            return r;
        }
    }
    req.url = base_ + req.url;
    req.headers.emplace_back("Authorization", "Bearer " + token);
    HttpResponse r = http_->send(req);
    if (r.status == 0) setOnline(false, "offline");
    else if (r.status == 401) {
        std::string code = errorCode(r);
        if (code != "token_expired" && code != "reauth_required") code = "token_invalid";
        clearLinkIfToken(token, code);
        setOnline(true, code);
    } else setOnline(true, "");
    return r;
}

HttpResponse CloudClient::api(HttpRequest req, const BodySink* sink, const std::atomic<bool>* cancel) {
    std::string token;
    {
        std::lock_guard<std::mutex> lk(m_);
        token = store_.token;
    }
    if (token.empty()) {
        HttpResponse r;
        r.error = "not_linked";
        return r;
    }
    req.url = base_ + req.url;
    req.headers.emplace_back("Authorization", "Bearer " + token);
    HttpResponse r = http_->send(req, sink, cancel);
    if (r.cancelled || r.sinkFailed) return r;
    if (r.status == 0) {
        setOnline(false, "offline");
    } else if (r.status == 401) {
        std::string code = errorCode(r);
        if (code != "token_expired" && code != "reauth_required") code = "token_invalid";
        // Only drop the token this request used (a re-link may have replaced it meanwhile).
        clearLinkIfToken(token, code);
        setOnline(true, code);
    } else {
        setOnline(true, "");
    }
    return r;
}

CloudClient::MeResult CloudClient::checkToken(const std::string& token) {
    MeResult res;
    HttpRequest req;
    req.url = base_ + "/api/devices/me";
    req.headers.emplace_back("Authorization", "Bearer " + token);
    req.timeoutMs = 10000;
    const HttpResponse r = http_->send(req);
    res.status = r.status;
    if (r.status == 0) {
        res.error = "offline";
        setOnline(false, "offline");
        return res;
    }
    setOnline(true, "");
    if (r.status == 401) {
        res.error = "token_invalid";
        return res;
    }
    if (r.status != 200) {
        res.error = "http_error";
        return res;
    }
    const json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) {
        res.error = "http_error";
        return res;
    }
    // accountId is a string on the wire; tolerate a number.
    if (auto a = j.find("accountId"); a != j.end() && a->is_number_integer()) res.info.accountId = std::to_string(a->get<int64_t>());
    else res.info.accountId = str(j, "accountId");
    res.info.accountName = str(j, "accountName");
    res.info.expiresAt = str(j, "expiresAt");
    if (!AppPaths::isValidAccountId(res.info.accountId)) res.error = "http_error";
    return res;
}

bool CloudClient::storeLink(const std::string& token, const LinkInfo& info, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    CloudStore next = store_;
    next.link = info;
    next.token = token;
    if (!next.save(file_)) {
        err = "cannot protect/store the token (DPAPI or cloud.json write failed)";
        return false;
    }
    store_ = std::move(next);
    error_.clear();
    online_ = true;
    version_.fetch_add(1);
    return true;
}

void CloudClient::clearLink(const std::string& errorCode) {
    std::lock_guard<std::mutex> lk(m_);
    const bool had = store_.link.has_value();
    store_.link.reset();
    store_.token.clear();
    store_.save(file_);
    if (had || error_ != errorCode) {
        error_ = errorCode;
        version_.fetch_add(1);
    }
}

bool CloudClient::clearLinkIfToken(const std::string& token, const std::string& errorCode) {
    std::lock_guard<std::mutex> lk(m_);
    if (store_.token.empty() || store_.token != token) return false;
    store_.link.reset();
    store_.token.clear();
    store_.save(file_);
    error_ = errorCode;
    version_.fetch_add(1);
    return true;
}

void CloudClient::deleteDevice(const std::string& token) {
    if (token.empty()) return;
    HttpRequest req;
    req.method = "DELETE";
    req.url = base_ + "/api/devices/me";
    req.headers.emplace_back("Authorization", "Bearer " + token);
    req.timeoutMs = 5000;
    http_->send(req);
}

} // namespace ks::cloud
