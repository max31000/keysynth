// macOS WebSocket client for the relay (ARCHITECTURE §15 *Relay*): NSURLSessionWebSocketTask — TLS, system proxy,
// no new dependency (the counterpart of WinHttpWebSocket.cpp). Contract: platform/WinHttpWebSocket.h.
//
// Threading: NSURLSession calls back on its own serial delegate queue, never on the thread calling open() / send()
// / close() / abort(). Callback state lives in a shared Core object; the socket's destructor marks it dead and waits
// for an in-flight callback (cbMutex), so no user callback runs after the WebSocket is destroyed.

#include "platform/WinHttpWebSocket.h"

#import <Foundation/Foundation.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace {

using ks::platform::WebSocket;

constexpr NSInteger kMaxMessage = 4 << 20; // inbound message cap (relay frames are <= 1 MB)
constexpr size_t kMaxQueue = 16u << 20;    // outbound queue hard cap

struct Core {
    std::mutex m;
    std::condition_variable cv;
    enum class Phase { Idle, Opening, Open, Closed } phase = Phase::Idle;
    bool opened = false, failed = false, aborted = false, closeReported = false;
    int httpStatus = 0;
    std::string error;
    size_t queued = 0;

    std::recursive_mutex cbMutex; // held while a user callback runs
    bool dead = false;            // set (under cbMutex) by ~MacWebSocket
    WebSocket::OnText onText;
    WebSocket::OnClose onClose;

    void deliverText(std::string text) {
        std::lock_guard<std::recursive_mutex> lk(cbMutex);
        if (!dead && onText) onText(std::move(text));
    }
    // Exactly once per opened socket.
    void reportClose(int code, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lk(m);
            const bool wasOpen = opened;
            phase = Phase::Closed;
            cv.notify_all();
            if (!wasOpen || closeReported) return;
            closeReported = true;
        }
        std::lock_guard<std::recursive_mutex> lk(cbMutex);
        if (!dead && onClose) onClose(code, reason);
    }
};

int statusOf(NSURLResponse* r) {
    return [r isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<int>(((NSHTTPURLResponse*)r).statusCode) : 0;
}

} // namespace

@interface KsWsDelegate : NSObject <NSURLSessionWebSocketDelegate>
@end

@implementation KsWsDelegate {
  @public
    std::shared_ptr<Core> core;
}

- (void)URLSession:(NSURLSession*)session
          webSocketTask:(NSURLSessionWebSocketTask*)task
    didOpenWithProtocol:(NSString*)protocol {
    (void)session;
    (void)protocol;
    std::lock_guard<std::mutex> lk(core->m);
    core->httpStatus = 101;
    (void)task;
    if (core->phase == Core::Phase::Opening) {
        core->phase = Core::Phase::Open;
        core->opened = true;
    }
    core->cv.notify_all();
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    std::shared_ptr<Core> c = core;
    bool wasOpen;
    {
        std::lock_guard<std::mutex> lk(c->m);
        wasOpen = c->opened;
        if (!wasOpen) {
            c->failed = true;
            c->httpStatus = statusOf(task.response);
            c->error = error ? std::string(error.localizedDescription.UTF8String ?: "connection failed")
                             : std::string("connection closed during the handshake");
            c->phase = Core::Phase::Closed;
            c->cv.notify_all();
        }
    }
    if (wasOpen) {
        auto* ws = (NSURLSessionWebSocketTask*)task;
        const NSInteger code = ws.closeCode;
        NSString* reason = ws.closeReason ? [[NSString alloc] initWithData:ws.closeReason
                                                                  encoding:NSUTF8StringEncoding]
                                          : nil;
        c->reportClose(code == NSURLSessionWebSocketCloseCodeInvalid ? ks::platform::kWsCloseAbnormal
                                                                      : static_cast<int>(code),
                       reason ? std::string(reason.UTF8String ?: "") : std::string());
    }
}

- (void)URLSession:(NSURLSession*)session
       webSocketTask:(NSURLSessionWebSocketTask*)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode
              reason:(NSData*)reason {
    (void)session;
    (void)task;
    NSString* r = reason ? [[NSString alloc] initWithData:reason encoding:NSUTF8StringEncoding] : nil;
    core->reportClose(closeCode == NSURLSessionWebSocketCloseCodeInvalid ? ks::platform::kWsCloseAbnormal
                                                                         : static_cast<int>(closeCode),
                      r ? std::string(r.UTF8String ?: "") : std::string());
}
@end

namespace {

class MacWebSocket final : public WebSocket {
public:
    MacWebSocket() : core_(std::make_shared<Core>()) {}

    ~MacWebSocket() override {
        close(ks::platform::kWsCloseGoingAway, "");
        {
            // Give the peer a moment to answer the close frame (as the WinHTTP client does).
            std::unique_lock<std::mutex> lk(core_->m);
            core_->cv.wait_for(lk, std::chrono::seconds(2), [&] { return core_->phase != Core::Phase::Open; });
        }
        {
            std::lock_guard<std::recursive_mutex> lk(core_->cbMutex); // waits for a running callback
            core_->dead = true;
        }
        [task_ cancel];
        [session_ invalidateAndCancel]; // releases the delegate (NSURLSession retains it)
    }

    ks::platform::WsOpenResult open(const ks::platform::WsHandshake& hs, OnText onText, OnClose onClose) override {
        ks::platform::WsOpenResult r;
        core_->onText = std::move(onText);
        core_->onClose = std::move(onClose);
        NSString* urlString = [NSString stringWithUTF8String:hs.url.c_str()];
        NSURL* url = urlString ? [NSURL URLWithString:urlString] : nil;
        if (url == nil) {
            r.error = "bad url";
            return r;
        }
        // http(s) URLs are accepted like the WinHTTP client does; NSURLSession wants ws(s).
        NSURLComponents* uc = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
        if ([uc.scheme isEqualToString:@"http"]) uc.scheme = @"ws";
        else if ([uc.scheme isEqualToString:@"https"]) uc.scheme = @"wss";
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:uc.URL];
        // A task's timeout is an *idle* timeout (and the request's wins over the session's): keep it long, the relay
        // heartbeat closes a dead link earlier. The handshake timeout is the wait below.
        req.timeoutInterval = 600;
        for (const auto& [k, v] : hs.headers)
            [req setValue:[NSString stringWithUTF8String:v.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:k.c_str()]];

        {
            std::lock_guard<std::mutex> lk(core_->m);
            if (core_->aborted) {
                r.error = "aborted";
                return r;
            }
            core_->phase = Core::Phase::Opening;
        }
        KsWsDelegate* delegate = [[KsWsDelegate alloc] init];
        delegate->core = core_;
        NSOperationQueue* queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1; // serial callbacks
        NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = 600;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:cfg delegate:delegate delegateQueue:queue];
        NSURLSessionWebSocketTask* task = [session webSocketTaskWithRequest:req];
        task.maximumMessageSize = kMaxMessage;
        {
            std::lock_guard<std::mutex> lk(core_->m); // abort() may read task_ from another thread
            session_ = session;
            task_ = task;
        }
        [task resume];

        std::unique_lock<std::mutex> lk(core_->m);
        const bool done = core_->cv.wait_for(lk, std::chrono::milliseconds(hs.timeoutMs), [&] {
            return core_->opened || core_->failed || core_->aborted;
        });
        if (core_->opened) {
            r.ok = true;
            r.httpStatus = 101;
            lk.unlock();
            receive();
            return r;
        }
        r.httpStatus = core_->httpStatus;
        r.error = core_->aborted ? "aborted" : !done ? "handshake timeout" : core_->error;
        if (r.error.empty()) r.error = "connection failed";
        core_->phase = Core::Phase::Closed;
        lk.unlock();
        [task_ cancel];
        return r;
    }

    bool send(std::string text) override {
        const size_t n = text.size();
        {
            std::lock_guard<std::mutex> lk(core_->m);
            if (core_->phase != Core::Phase::Open || core_->queued + n > kMaxQueue) return false;
            core_->queued += n;
        }
        NSString* s = [[NSString alloc] initWithBytes:text.data() length:n encoding:NSUTF8StringEncoding];
        if (s == nil) { // not UTF-8: never valid as a text frame
            std::lock_guard<std::mutex> lk(core_->m);
            core_->queued -= n;
            return false;
        }
        std::shared_ptr<Core> c = core_;
        NSURLSessionWebSocketMessage* msg = [[NSURLSessionWebSocketMessage alloc] initWithString:s];
        [task_ sendMessage:msg
            completionHandler:^(NSError* error) {
              (void)error; // a failed send also fails the receive loop, which reports the close
              std::lock_guard<std::mutex> lk(c->m);
              c->queued -= n;
            }];
        return true;
    }

    void close(int code, const std::string& reason) override {
        {
            std::lock_guard<std::mutex> lk(core_->m);
            if (core_->phase != Core::Phase::Open) return;
        }
        NSData* r = [NSData dataWithBytes:reason.data() length:reason.size()];
        [task_ cancelWithCloseCode:static_cast<NSURLSessionWebSocketCloseCode>(code) reason:r];
    }

    void abort() override {
        NSURLSessionWebSocketTask* task = nil;
        {
            std::lock_guard<std::mutex> lk(core_->m);
            core_->aborted = true;
            core_->cv.notify_all();
            task = task_;
        }
        [task cancel]; // an open socket: the receive loop fails -> onClose(1006) on the delegate queue
    }

    size_t queuedBytes() const override {
        std::lock_guard<std::mutex> lk(core_->m);
        return core_->queued;
    }

private:
    void receive() { receiveOn(task_, core_); }

    static void receiveOn(NSURLSessionWebSocketTask* task, std::shared_ptr<Core> c) {
        __weak NSURLSessionWebSocketTask* weakTask = task;
        [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* msg, NSError* error) {
          NSURLSessionWebSocketTask* t = weakTask;
          if (error != nil || msg == nil) {
              const NSInteger code = t ? t.closeCode : NSURLSessionWebSocketCloseCodeInvalid;
              c->reportClose(code == NSURLSessionWebSocketCloseCodeInvalid
                                 ? (error.code == EMSGSIZE ? ks::platform::kWsCloseTooBig : ks::platform::kWsCloseAbnormal)
                                 : static_cast<int>(code),
                             "");
              return;
          }
          if (msg.type == NSURLSessionWebSocketMessageTypeString) c->deliverText(std::string(msg.string.UTF8String ?: ""));
          if (t) receiveOn(t, c);
        }];
    }

    std::shared_ptr<Core> core_;
    NSURLSession* session_ = nil;
    NSURLSessionWebSocketTask* task_ = nil;
};

} // namespace

namespace ks::platform {

std::unique_ptr<WebSocket> makeDefaultWebSocket() { return std::make_unique<MacWebSocket>(); }

} // namespace ks::platform
