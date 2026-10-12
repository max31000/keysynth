#pragma once
// HTTP layer of the cloud client (ARCHITECTURE §15). One blocking call per request, made from worker threads
// only (never the audio or message thread). Production: JuceHttpTransport (WinINet); tests: fakes.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ks::cloud {

struct HttpRequest {
    std::string method = "GET";
    std::string url;                                          // absolute, already percent-encoded
    std::vector<std::pair<std::string, std::string>> headers; // e.g. {"If-Match", "\"4\""}
    std::string body;                                         // sent when non-empty (Content-Type: application/json)
    int timeoutMs = 15000;
};

struct HttpResponse {
    int status = 0;                             // 0 = no response (network error, cancelled)
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;                           // when no sink consumed it (bounded)
    std::string error;                          // transport error text (status 0)
    bool cancelled = false;                     // the cancel flag stopped it
    bool sinkFailed = false;                    // the sink returned false (disk full, ...)

    std::string header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
    bool ok() const noexcept { return status >= 200 && status < 300; }
};

// Receives body bytes of 2xx responses (`head`: status + headers, empty body); returning false aborts the
// transfer (HttpResponse::sinkFailed).
using BodySink = std::function<bool(const HttpResponse& head, const char* data, size_t n)>;

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Thread-safe. `sink` (optional) streams 2xx bodies instead of collecting them; `cancel` (optional) is polled
    // between chunks.
    virtual HttpResponse send(const HttpRequest& req, const BodySink* sink = nullptr,
                              const std::atomic<bool>* cancel = nullptr) = 0;
};

std::shared_ptr<HttpTransport> makeJuceHttpTransport();

// Percent-encode one path segment (RFC 3986 unreserved kept; everything else incl. '/' encoded).
std::string encodePathSegment(const std::string& s);
// Encode a '/'-separated relative path segment by segment.
std::string encodePath(const std::string& relPath);

} // namespace ks::cloud
