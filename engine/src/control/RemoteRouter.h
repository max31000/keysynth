#pragma once
// RemoteRouter (ARCHITECTURE §15 *Relay*): outbound side of the relayed controllers (virtual clients "r:<cid>").
// Turns ControlServer deliveries into relay envelopes {t:"msg", to, except?, lossy?, msg} and applies the remote
// rates: telemetry <= 10 Hz, midi notes merged <= 15 Hz, `param` coalesced per (node,param) <= 20 Hz; telemetry and
// midi only reach active (not paused) controllers. Message thread only; no IO (the sink queues the frame).

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ks {

class RemoteRouter {
public:
    using Clock = std::chrono::steady_clock;
    using Sink = std::function<void(const nlohmann::json& envelope, bool lossy)>;

    static constexpr const char* kPrefix = "r:"; // ControlServer client id = kPrefix + cid
    static constexpr int kTelemetryIntervalMs = 100;
    static constexpr int kMidiIntervalMs = 67;
    static constexpr int kParamIntervalMs = 50;

    static bool isRemoteId(const std::string& clientId);
    static std::string cidOf(const std::string& clientId); // "" when not remote
    static bool isLossy(const std::string& type);         // telemetry, midi, samples_progress

    void setSink(Sink s) { sink_ = std::move(s); }
    void setClock(std::function<Clock::time_point()> c) { clock_ = std::move(c); }

    // Membership (ControlServer keeps the client table; these track per-controller state).
    void join(const std::string& cid);
    void leave(const std::string& cid);
    void clear();
    void setPaused(const std::string& cid, bool paused);
    bool paused(const std::string& cid) const;
    size_t count() const noexcept { return ctl_.size(); }
    size_t activeCount() const;

    // Deliveries.
    void reply(const std::string& cid, const nlohmann::json& msg);
    // To every remote controller except `exceptCid` ("" = none).
    void broadcast(const nlohmann::json& msg, const std::string& exceptCid = {});
    // Flush coalesced params and the midi batch when due (pump, 30 Hz).
    void tick();

private:
    struct Ctl {
        bool paused = false;
    };
    struct PendingParam {
        nlohmann::json msg;
        std::string except;
        bool pending = false;
        Clock::time_point last{};
    };
    Clock::time_point now() const { return clock_ ? clock_() : Clock::now(); }
    void emit(const std::string& to, const std::string& except, bool lossy, const nlohmann::json& msg);
    void emitActive(bool lossy, const nlohmann::json& msg); // to "*" or each active cid
    void flushMidi(Clock::time_point t);

    Sink sink_;
    std::function<Clock::time_point()> clock_;
    std::map<std::string, Ctl> ctl_;
    Clock::time_point lastTelemetry_{}, lastMidi_{};
    bool sentTelemetry_ = false, sentMidi_ = false;
    nlohmann::json midiNotes_ = nlohmann::json::array();
    std::map<std::string, PendingParam> params_; // "<node>/<param>"
};

} // namespace ks
