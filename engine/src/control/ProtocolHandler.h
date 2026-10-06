#pragma once
// Protocol handler layer (docs/PROTOCOL.md): JSON request -> Session calls -> outgoing messages.
// No networking here, so it is unit-testable. Never throws; bad input yields `error` replies.

#include "control/Session.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace ks {

struct Outgoing {
    enum class Target { Reply, Broadcast, Others };
    Target target = Target::Reply;
    nlohmann::json message;
};

class ProtocolHandler {
public:
    explicit ProtocolHandler(Session& session) : session_(session) {}

    std::vector<Outgoing> handleText(const std::string& text);
    std::vector<Outgoing> handle(const nlohmann::json& msg);

    static nlohmann::json error(const nlohmann::json& id, const std::string& code, const std::string& message);

private:
    Session& session_;
};

} // namespace ks
