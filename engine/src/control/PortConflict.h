#pragma once
// Port conflict classification for the control server (ARCHITECTURE §11 "Port conflicts", §16 "Port busy at
// start"). Pure functions over the listener table (platform/PortOwner), so tests can feed fake tables.

#include "platform/PortOwner.h"

#include <optional>
#include <string>
#include <vector>

namespace ks {

enum class PortOwnerKind {
    Unknown,  // the bind failed but no listener was found (e.g. an excluded port range, permissions)
    Keysynth, // another keysynth engine (outside our single instance: headless/dev build, other session)
    Foreign,  // any other program (a dev mock, another server)
};

struct PortConflict {
    int port = 0;
    PortOwnerKind kind = PortOwnerKind::Unknown;
    unsigned long pid = 0;
    std::string name;    // executable file name, "" when unknown
    std::string path;    // full executable path, "" when unknown
    std::string address; // the conflicting listener's address
    std::string error;   // bind error (kind Unknown)
};

// A listener on 127.0.0.1, 0.0.0.0 or :: (maybe dual-stack) takes connections to 127.0.0.1:<port>.
bool blocksLoopback(const platform::TcpListener& l);
// `keysynth-engine.exe` (both the console and the tray build), case-insensitive.
bool isKeysynthImage(const std::string& image);
// First blocking listener → conflict (keysynth owners win over foreign ones when several listen); nullopt = free.
std::optional<PortConflict> findConflict(int port, const std::vector<platform::TcpListener>& listeners);
// Listeners that do not block the bind but can still steal `localhost` clients ([::1]).
std::vector<platform::TcpListener> shadowListeners(const std::vector<platform::TcpListener>& listeners);

// User-facing texts (tray balloon / tooltip / log). Russian or English.
std::string conflictMessage(const PortConflict& c, bool russian);
std::string portFreeMessage(int port, bool russian);
// One line for logs: "port 7341: node.exe (PID 1234, C:\...\node.exe) on 127.0.0.1".
std::string conflictLogLine(const PortConflict& c);

} // namespace ks
