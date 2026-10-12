#include "platform/WinHttpWebSocket.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace ks::platform {

std::string wsToHttpUrl(const std::string& url) {
    if (url.rfind("wss://", 0) == 0) return "https://" + url.substr(6);
    if (url.rfind("ws://", 0) == 0) return "http://" + url.substr(5);
    return url;
}

#if defined(_WIN32)
namespace {

constexpr size_t kMaxMessage = 4u << 20;     // inbound message cap (relay frames are <= 1 MB)
constexpr size_t kMaxQueue = 16u << 20;      // outbound queue hard cap
constexpr DWORD kIdleReceiveTimeoutMs = 600000; // after the upgrade; the relay heartbeat closes earlier

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string lastError(const char* what) { return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")"; }

class WinHttpWebSocket final : public WebSocket {
public:
    ~WinHttpWebSocket() override {
        close(kWsCloseGoingAway, "");
        {
            // Give the peer a moment to answer the close frame, then abort the blocking receive.
            std::unique_lock<std::mutex> lk(m_);
            if (open_) cv_.wait_for(lk, std::chrono::seconds(2), [&] { return recvDone_; });
        }
        abortHandle();
        if (recv_.joinable()) recv_.join();
        {
            std::lock_guard<std::mutex> lk(m_);
            finished_ = true;
        }
        cv_.notify_all();
        if (send_.joinable()) send_.join();
        if (ws_) WinHttpCloseHandle(ws_);
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
    }

    WsOpenResult open(const WsHandshake& hs, OnText onText, OnClose onClose) override {
        WsOpenResult r;
        onText_ = std::move(onText);
        onClose_ = std::move(onClose);
        const std::wstring url = widen(wsToHttpUrl(hs.url));
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
            r.error = "bad url";
            return r;
        }
        const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

        // Automatic proxy (WPAD / PAC / system settings) needs Windows 8.1; fall back to the WinHTTP default.
        session_ = WinHttpOpen(L"keysynth-engine", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            session_ = WinHttpOpen(L"keysynth-engine", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            r.error = lastError("WinHttpOpen");
            return r;
        }
        const int t = hs.timeoutMs > 0 ? hs.timeoutMs : 10000;
        WinHttpSetTimeouts(session_, t, t, t, t);
        connect_ = WinHttpConnect(session_, host.c_str(), uc.nPort, 0);
        if (!connect_) {
            r.error = lastError("WinHttpConnect");
            return r;
        }
        HINTERNET req = WinHttpOpenRequest(connect_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            r.error = lastError("WinHttpOpenRequest");
            return r;
        }
        auto closeReq = [&] {
            if (req) WinHttpCloseHandle(req);
            req = nullptr;
        };
        if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            r.error = lastError("WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET");
            closeReq();
            return r;
        }
        for (const auto& [k, v] : hs.headers) {
            const std::wstring line = widen(k + ": " + v);
            if (!WinHttpAddRequestHeaders(req, line.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
                r.error = lastError("WinHttpAddRequestHeaders");
                closeReq();
                return r;
            }
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            r.error = lastError("WebSocket handshake");
            closeReq();
            return r;
        }
        DWORD status = 0, len = sizeof(status);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &len, WINHTTP_NO_HEADER_INDEX))
            r.httpStatus = static_cast<int>(status);
        if (status != 101) {
            r.error = "upgrade refused (HTTP " + std::to_string(status) + ")";
            closeReq();
            return r;
        }
        DWORD idle = kIdleReceiveTimeoutMs;
        WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &idle, sizeof(idle));
        ws_ = WinHttpWebSocketCompleteUpgrade(req, 0);
        closeReq();
        if (!ws_) {
            r.error = lastError("WinHttpWebSocketCompleteUpgrade");
            return r;
        }
        WinHttpSetOption(ws_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &idle, sizeof(idle));
        {
            std::lock_guard<std::mutex> lk(m_);
            open_ = true;
        }
        recv_ = std::thread([this] { receiveLoop(); });
        send_ = std::thread([this] { sendLoop(); });
        r.ok = true;
        return r;
    }

    bool send(std::string text) override {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!open_ || closeRequested_ || finished_) return false;
            if (queued_ + text.size() > kMaxQueue) return false;
            queued_ += text.size();
            queue_.push_back(std::move(text));
        }
        cv_.notify_all();
        return true;
    }

    void close(int code, const std::string& reason) override {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!open_ || closeRequested_) return;
            closeRequested_ = true;
            closeCode_ = code;
            closeReason_ = reason.substr(0, 120);
            queue_.clear();
            queued_ = 0;
        }
        cv_.notify_all();
    }

    size_t queuedBytes() const override {
        std::lock_guard<std::mutex> lk(m_);
        return queued_;
    }

private:
    void abortHandle() {
        std::lock_guard<std::mutex> lk(handleM_);
        if (ws_ && !aborted_) {
            aborted_ = true;
            // Cancels a pending blocking receive/send on the other threads.
            WinHttpCloseHandle(ws_);
            ws_ = nullptr;
        }
    }

    HINTERNET wsHandle() {
        std::lock_guard<std::mutex> lk(handleM_);
        return ws_;
    }

    void finish(int code, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (finished_) return;
            finished_ = true;
            queue_.clear();
            queued_ = 0;
        }
        cv_.notify_all();
        if (onClose_) onClose_(code, reason);
    }

    void receiveLoop() {
        std::string msg;
        std::vector<char> buf(64 * 1024);
        bool discard = false;
        for (;;) {
            HINTERNET ws = wsHandle();
            if (!ws) {
                finish(kWsCloseAbnormal, "aborted");
                break;
            }
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD err = WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &read, &type);
            if (err != NO_ERROR) {
                finish(kWsCloseAbnormal, "receive failed (" + std::to_string(err) + ")");
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT status = kWsCloseAbnormal;
                char reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
                DWORD rlen = 0;
                if (WinHttpWebSocketQueryCloseStatus(ws, &status, reason, sizeof(reason), &rlen) != NO_ERROR) rlen = 0;
                bool weSent;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    weSent = shutdownSent_;
                    shutdownSent_ = true;
                }
                if (!weSent) { // answer the peer's close
                    std::lock_guard<std::mutex> sl(sendM_);
                    WinHttpWebSocketShutdown(ws, status, nullptr, 0);
                }
                finish(status, std::string(reason, rlen));
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                continue; // the relay protocol is text only
            if (!discard) msg.append(buf.data(), read);
            if (msg.size() > kMaxMessage) {
                discard = true;
                msg.clear();
            }
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                if (discard) {
                    close(kWsCloseTooBig, "message too big");
                    discard = false;
                } else if (onText_) {
                    onText_(std::move(msg));
                }
                msg.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            recvDone_ = true;
        }
        cv_.notify_all();
    }

    void sendLoop() {
        for (;;) {
            std::string text;
            int code = 0;
            std::string reason;
            bool doShutdown = false;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return finished_ || !queue_.empty() || (closeRequested_ && !shutdownSent_); });
                if (finished_) return;
                if (closeRequested_ && !shutdownSent_) {
                    shutdownSent_ = true;
                    doShutdown = true;
                    code = closeCode_;
                    reason = closeReason_;
                } else {
                    text = std::move(queue_.front());
                    queue_.pop_front();
                    queued_ -= text.size();
                }
            }
            HINTERNET ws = wsHandle();
            if (!ws) return;
            std::unique_lock<std::mutex> sl(sendM_); // one send operation at a time (shutdown counts as one)
            if (doShutdown) {
                // Sends our close frame; the receive loop gets the peer's answer (or the abort) and finishes.
                if (WinHttpWebSocketShutdown(ws, static_cast<USHORT>(code), reason.empty() ? nullptr : reason.data(),
                                             static_cast<DWORD>(reason.size())) != NO_ERROR) {
                    sl.unlock();
                    abortHandle();
                }
                continue;
            }
            if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text.data(),
                                     static_cast<DWORD>(text.size())) != NO_ERROR) {
                sl.unlock();
                abortHandle(); // broken connection: the receive loop reports the close
                return;
            }
        }
    }

    HINTERNET session_ = nullptr, connect_ = nullptr;
    std::mutex handleM_, sendM_;
    HINTERNET ws_ = nullptr;
    bool aborted_ = false;
    OnText onText_;
    OnClose onClose_;
    std::thread recv_, send_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    size_t queued_ = 0;
    bool open_ = false, closeRequested_ = false, shutdownSent_ = false, finished_ = false, recvDone_ = false;
    int closeCode_ = kWsCloseNormal;
    std::string closeReason_;
};

} // namespace

std::unique_ptr<WebSocket> makeWinHttpWebSocket() { return std::make_unique<WinHttpWebSocket>(); }

#else

std::unique_ptr<WebSocket> makeWinHttpWebSocket() { return nullptr; }

#endif

} // namespace ks::platform
