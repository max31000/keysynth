#pragma once
// Outbound WebSocket client (ARCHITECTURE §15 *Relay*). Abstract `WebSocket` so tests use a fake; production =
// WinHTTP on Windows, NSURLSessionWebSocketTask on macOS (TLS, system/automatic proxy, no new dependency). Never used on the audio thread.
//
// Threading: open() blocks (handshake) on the caller's thread. After a successful open the implementation runs a
// receive thread (invokes onText / onClose) and a send thread (drains the queue filled by send()). send() and
// close() are thread-safe and never block on the network. onClose is invoked exactly once per opened socket (also
// after close()); callbacks must not destroy the socket.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ks::platform {

struct WsHandshake {
    std::string url; // ws:// or wss:// (http:// / https:// accepted too)
    std::vector<std::pair<std::string, std::string>> headers; // e.g. {"Authorization", "Bearer ..."}
    int timeoutMs = 10000;                                    // resolve / connect / handshake
};

struct WsOpenResult {
    bool ok = false;
    int httpStatus = 0; // status of the upgrade response (101 on success; 0 = no response)
    std::string error;  // transport error text when !ok
};

// RFC 6455 close codes used here.
inline constexpr int kWsCloseNormal = 1000;
inline constexpr int kWsCloseGoingAway = 1001;
inline constexpr int kWsCloseAbnormal = 1006; // no close frame (network error, receive failure)
inline constexpr int kWsCloseTooBig = 1009;

class WebSocket {
public:
    using OnText = std::function<void(std::string text)>;
    using OnClose = std::function<void(int code, const std::string& reason)>;

    virtual ~WebSocket() = default;
    // Blocking handshake. Call once.
    virtual WsOpenResult open(const WsHandshake& hs, OnText onText, OnClose onClose) = 0;
    // Queue a text frame. False when the socket is not open (closed/closing) or the queue is over its hard cap.
    virtual bool send(std::string text) = 0;
    // Start the closing handshake (or abort a dead connection). onClose follows.
    virtual void close(int code = kWsCloseNormal, const std::string& reason = {}) = 0;
    // Any thread: cancel a blocking open() or drop the connection without a closing handshake. onClose follows
    // for an opened socket. Never invokes callbacks on the calling thread.
    virtual void abort() = 0;
    // Bytes waiting in the send queue.
    virtual size_t queuedBytes() const = 0;
};

using WebSocketFactory = std::function<std::unique_ptr<WebSocket>()>;

std::unique_ptr<WebSocket> makeWinHttpWebSocket(); // Windows only (nullptr elsewhere)
// The platform's client: WinHTTP (Windows), NSURLSessionWebSocketTask (macOS, WebSocket_mac.mm); nullptr elsewhere.
std::unique_ptr<WebSocket> makeDefaultWebSocket();

// "wss://h/p" -> "https://h/p", "ws://" -> "http://"; others unchanged.
std::string wsToHttpUrl(const std::string& url);

} // namespace ks::platform
