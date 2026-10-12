#pragma once
// Protocol handler layer (docs/PROTOCOL.md): JSON request -> Session calls -> outgoing messages.
// No networking here, so it is unit-testable. Never throws; bad input yields `error` replies.
// Every request is checked against the session's trust first (control/Trust.h): `web` sessions are default-deny.

#include "control/Session.h"
#include "control/Trust.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace ks::cloud {
class CloudServices;
}

namespace ks {

struct Outgoing {
    enum class Target { Reply, Broadcast, Others };
    Target target = Target::Reply;
    nlohmann::json message;
};

// The connection a request came from.
struct ClientInfo {
    std::string id;            // ControlServer connection id ("" in tests)
    Trust trust = Trust::Local;
};

// An outgoing message produced asynchronously (cloud replies/events), addressed relative to `clientId`.
struct AddressedOutgoing {
    std::string clientId; // Reply -> this client, Others -> everyone else; ignored for Broadcast
    Outgoing out;
};

// Engine version (CMake project version, no "v").
const char* engineVersion();

class ProtocolHandler {
public:
    explicit ProtocolHandler(Session& session) : session_(session) {}

    std::vector<Outgoing> handleText(const std::string& text, const ClientInfo& client = {});
    std::vector<Outgoing> handle(const nlohmann::json& msg, const ClientInfo& client = {});

    // Optional cloud services (cloud_*, sync_now, samples_*; hello's `cloud` field).
    void setCloud(cloud::CloudServices* c) noexcept { cloud_ = c; }
    cloud::CloudServices* cloud() const noexcept { return cloud_; }

    static nlohmann::json error(const nlohmann::json& id, const std::string& code, const std::string& message);

private:
    Session& session_;
    cloud::CloudServices* cloud_ = nullptr;
};

} // namespace ks
