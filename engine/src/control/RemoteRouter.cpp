#include "control/RemoteRouter.h"

using nlohmann::json;

namespace ks {

namespace {
std::string typeOf(const json& m) {
    auto it = m.find("type");
    return it != m.end() && it->is_string() ? it->get<std::string>() : std::string();
}
} // namespace

bool RemoteRouter::isRemoteId(const std::string& id) { return id.rfind(kPrefix, 0) == 0 && id.size() > 2; }

std::string RemoteRouter::cidOf(const std::string& id) { return isRemoteId(id) ? id.substr(2) : std::string(); }

bool RemoteRouter::isLossy(const std::string& type) {
    return type == "telemetry" || type == "midi" || type == "samples_progress";
}

void RemoteRouter::join(const std::string& cid) { ctl_.try_emplace(cid); }

void RemoteRouter::leave(const std::string& cid) {
    ctl_.erase(cid);
    if (ctl_.empty()) clear();
}

void RemoteRouter::clear() {
    ctl_.clear();
    params_.clear();
    midiNotes_ = json::array();
}

void RemoteRouter::setPaused(const std::string& cid, bool p) {
    auto it = ctl_.find(cid);
    if (it != ctl_.end()) it->second.paused = p;
}

bool RemoteRouter::paused(const std::string& cid) const {
    auto it = ctl_.find(cid);
    return it != ctl_.end() && it->second.paused;
}

size_t RemoteRouter::activeCount() const {
    size_t n = 0;
    for (const auto& [cid, c] : ctl_)
        if (!c.paused) ++n;
    return n;
}

void RemoteRouter::emit(const std::string& to, const std::string& except, bool lossy, const json& msg) {
    if (!sink_) return;
    json env = {{"t", "msg"}, {"to", to}};
    if (!except.empty()) env["except"] = except;
    if (lossy) env["lossy"] = true;
    env["msg"] = msg;
    sink_(env, lossy);
}

void RemoteRouter::emitActive(bool lossy, const json& msg) {
    if (activeCount() == ctl_.size()) {
        emit("*", {}, lossy, msg);
        return;
    }
    for (const auto& [cid, c] : ctl_)
        if (!c.paused) emit(cid, {}, lossy, msg);
}

void RemoteRouter::reply(const std::string& cid, const json& msg) {
    if (cid.empty()) return;
    emit(cid, {}, isLossy(typeOf(msg)), msg);
}

void RemoteRouter::broadcast(const json& msg, const std::string& except) {
    if (ctl_.empty()) return;
    if (!except.empty() && ctl_.size() == 1 && ctl_.count(except)) return; // nobody else
    const std::string type = typeOf(msg);
    const auto t = now();
    if (type == "telemetry") {
        if (activeCount() == 0) return;
        if (sentTelemetry_ && t - lastTelemetry_ < std::chrono::milliseconds(kTelemetryIntervalMs)) return;
        sentTelemetry_ = true;
        lastTelemetry_ = t;
        emitActive(true, msg);
        return;
    }
    if (type == "midi") {
        if (activeCount() == 0) {
            midiNotes_ = json::array();
            return;
        }
        if (auto n = msg.find("notes"); n != msg.end() && n->is_array())
            for (const auto& x : *n) midiNotes_.push_back(x);
        flushMidi(t);
        return;
    }
    if (type == "param") {
        const std::string key = msg.value("node", json()).dump() + "/" + msg.value("param", json()).dump();
        PendingParam& p = params_[key];
        const bool due = p.last == Clock::time_point{} || t - p.last >= std::chrono::milliseconds(kParamIntervalMs);
        if (!p.pending && due) {
            p.last = t;
            emit("*", except, false, msg);
            return;
        }
        p.except = p.pending && p.except != except ? std::string() : except; // mixed origins: tell everyone
        p.msg = msg;
        p.pending = true;
        return;
    }
    if (!isLossy(type)) {
        // Keep order: a coalesced value must not arrive after a later event (fx removed, other preset...). A full
        // snapshot to everyone supersedes them; anything else flushes them first.
        if (type == "state" && except.empty())
            for (auto& [k, p] : params_) p.pending = false;
        else
            flushParams(t, false);
    }
    emit("*", except, isLossy(type), msg);
}

void RemoteRouter::flushParams(Clock::time_point t, bool onlyDue) {
    for (auto& [k, p] : params_) {
        if (!p.pending || (onlyDue && t - p.last < std::chrono::milliseconds(kParamIntervalMs))) continue;
        p.pending = false;
        p.last = t;
        if (!p.except.empty() && ctl_.size() == 1 && ctl_.count(p.except)) continue;
        emit("*", p.except, false, p.msg);
    }
}

void RemoteRouter::flushMidi(Clock::time_point t) {
    if (midiNotes_.empty()) return;
    if (sentMidi_ && t - lastMidi_ < std::chrono::milliseconds(kMidiIntervalMs)) return;
    sentMidi_ = true;
    lastMidi_ = t;
    json ev = {{"type", "midi"}, {"notes", std::move(midiNotes_)}};
    midiNotes_ = json::array();
    emitActive(true, ev);
}

void RemoteRouter::tick() {
    if (ctl_.empty()) return;
    const auto t = now();
    flushParams(t, true);
    if (activeCount() == 0) midiNotes_ = json::array();
    else flushMidi(t);
}

} // namespace ks
