// HttpTransport over JUCE WebInputStream (WinINet on Windows): HTTPS with the OS certificate store.
#include "cloud/HttpTransport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>

namespace ks::cloud {

std::string encodePathSegment(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hx[c >> 4]);
            out.push_back(hx[c & 15]);
        }
    }
    return out;
}

std::string encodePath(const std::string& relPath) {
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t slash = relPath.find('/', start);
        out += encodePathSegment(relPath.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        out.push_back('/');
        start = slash + 1;
    }
    return out;
}

namespace {

constexpr size_t kMaxCollectedBody = 4u << 20; // non-streamed bodies (JSON) are capped

class JuceHttpTransport final : public HttpTransport {
public:
    HttpResponse send(const HttpRequest& req, const BodySink* sink, const std::atomic<bool>* cancel) override {
        HttpResponse res;
        if (cancel && cancel->load()) {
            res.cancelled = true;
            res.error = "cancelled";
            return res;
        }
        juce::URL url(juce::String::fromUTF8(req.url.c_str()));
        if (!req.body.empty()) url = url.withPOSTData(juce::MemoryBlock(req.body.data(), req.body.size()));
        juce::String headers;
        bool hasContentType = false;
        for (const auto& [k, v] : req.headers) {
            headers << juce::String::fromUTF8(k.c_str()) << ": " << juce::String::fromUTF8(v.c_str()) << "\r\n";
            hasContentType = hasContentType || juce::String(k).equalsIgnoreCase("content-type");
        }
        if (!req.body.empty() && !hasContentType) headers << "Content-Type: application/json\r\n";

        juce::WebInputStream in(url, false);
        in.withExtraHeaders(headers)
            .withCustomRequestCommand(juce::String(req.method))
            .withConnectionTimeout(req.timeoutMs)
            .withNumRedirectsToFollow(0); // never forward the bearer token elsewhere
        if (!in.connect(nullptr) || in.isError()) {
            res.error = "connection failed";
            res.cancelled = cancel && cancel->load();
            return res;
        }
        res.status = in.getStatusCode();
        const juce::StringPairArray h = in.getResponseHeaders();
        for (int i = 0; i < h.size(); ++i) {
            std::string k = h.getAllKeys()[i].toStdString();
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            res.headers[k] = h.getAllValues()[i].toStdString();
        }
        const bool stream = sink != nullptr && res.ok();
        char buf[1 << 16];
        for (;;) {
            if (cancel && cancel->load()) {
                in.cancel();
                res.cancelled = true;
                res.status = 0;
                res.error = "cancelled";
                return res;
            }
            const int n = in.read(buf, static_cast<int>(sizeof(buf)));
            if (n < 0) {
                res.status = 0;
                res.error = "read failed";
                return res;
            }
            if (n == 0) {
                if (in.isExhausted()) break;
                // WinINet returns 0 only at the end or on a dropped connection.
                const auto total = in.getTotalLength();
                if (total >= 0 && in.getPosition() < total) {
                    res.status = 0;
                    res.error = "connection dropped";
                    return res;
                }
                break;
            }
            if (stream) {
                if (!(*sink)(res, buf, static_cast<size_t>(n))) {
                    in.cancel();
                    res.sinkFailed = true;
                    return res;
                }
            } else if (res.body.size() < kMaxCollectedBody) {
                res.body.append(buf, static_cast<size_t>(n));
            }
        }
        return res;
    }
};

} // namespace

std::shared_ptr<HttpTransport> makeJuceHttpTransport() { return std::make_shared<JuceHttpTransport>(); }

} // namespace ks::cloud
