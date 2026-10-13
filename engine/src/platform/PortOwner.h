#pragma once
// Who listens on a local TCP port (ARCHITECTURE §11 "Port conflicts"). Windows: GetExtendedTcpTable (IPv4 + IPv6,
// listeners only) → owning pid → image path. macOS: libproc (the user's own processes' sockets; others cannot be
// inspected without root). Other platforms: empty table. Message thread / tests only (a table
// read is a few syscalls; never on the audio thread).

#include "platform/DynamicLibrary.h" // currentProcessId

#include <string>
#include <vector>

namespace ks::platform {

struct TcpListener {
    std::string address; // "127.0.0.1", "0.0.0.0", "::", "::1", ...
    bool ipv6 = false;
    unsigned long pid = 0;
    std::string image; // executable file name ("node.exe"), "" when the process cannot be queried
    std::string path;  // full executable path (UTF-8), "" when unknown
};

// Every listening socket on `port` (both address families). Empty if the table cannot be read (logged once).
std::vector<TcpListener> tcpListeners(int port);
// Trial bind of exactly 127.0.0.1:<port> with SO_EXCLUSIVEADDRUSE (POSIX: without SO_REUSEADDR; closed right away): true when nothing that
// takes IPv4 loopback connections holds the port (a v6-only [::] listener does not count). `error` = WSA code text.
bool loopbackPortBindable(int port, std::string& error);
// Full image path of `pid` (UTF-8), "" if it cannot be opened (other user, exited, System).
std::string processImagePath(unsigned long pid);

} // namespace ks::platform
