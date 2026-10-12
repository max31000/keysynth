#pragma once
// Session trust (PROTOCOL.md "Sessions, Origin, Host"; ARCHITECTURE §11): the handshake's Origin decides whether a
// WS session is `local` (full protocol) or `web` (https://mvv42.ru: default-deny allow-list, logical paths only).
// `remote` = a controller relayed through the keysynth API (ARCHITECTURE §15 *Relay*): web rules + a deny list.
// Pure functions: unit-tested without sockets.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace ks {

enum class Trust { Local, Web, Remote };
const char* toString(Trust t);

struct OriginPolicy {
    bool devBuild = true;       // dev builds also accept the Vite dev server (:5173) and `extraWebOrigin`
    std::string extraWebOrigin; // --web-origin / KS_WEB_ORIGIN (dev builds only)
};

// nullopt = rejected. Empty origin (non-browser client) = local.
std::optional<Trust> classifyOrigin(const std::string& origin, int wsPort, int httpPort, const OriginPolicy& policy);
// Host header must be exactly 127.0.0.1:<port> or localhost:<port> (case-insensitive host).
bool hostAllowed(const std::string& host, int port);

// Logical path rules: relative, '/'-separated, no '..'/'.' or empty segment, no '\\', ':', control chars, leading
// '/', and starting with `prefix` (e.g. "userdata/presets/").
bool isLogicalPath(const std::string& p, const std::string& prefix);

// For `web` sessions: "" when `msg` (a request with a string `type`) is allowed, else the reason (-> `forbidden`).
std::string webRestriction(const nlohmann::json& msg, const char* sessionKind = "web");
// For `remote` sessions: webRestriction + deny cloud_link/unlink, audio device selection, samples_delete, plugins.
std::string remoteRestriction(const nlohmann::json& msg);
// Restriction for any trust ("" = allowed; `local` allows everything).
std::string trustRestriction(Trust t, const nlohmann::json& msg);

} // namespace ks
