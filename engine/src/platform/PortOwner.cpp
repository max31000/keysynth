#include "platform/PortOwner.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <libproc.h>
#include <netinet/in.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ks::platform {

#if defined(_WIN32)
namespace {

std::string utf8(const wchar_t* w, int len) {
    if (len <= 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

// GetExtendedTcpTable with the usual grow-and-retry (the table can change between the two calls).
std::vector<unsigned char> readTable(ULONG family) {
    std::vector<unsigned char> buf(16 * 1024);
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = static_cast<DWORD>(buf.size());
        const DWORD r = GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (r == NO_ERROR) return buf;
        if (r != ERROR_INSUFFICIENT_BUFFER) break;
        buf.resize(static_cast<size_t>(size) + 4096);
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "port owner: GetExtendedTcpTable failed (family %lu)\n", family);
        std::fflush(stderr);
    }
    return {};
}

void fillProcess(TcpListener& l) {
    l.path = processImagePath(l.pid);
    const auto slash = l.path.find_last_of("\\/");
    l.image = slash == std::string::npos ? l.path : l.path.substr(slash + 1);
    if (l.image.empty() && l.pid == 4) l.image = "System"; // http.sys (IIS, WinRM, ...) shows up as pid 4
}

} // namespace

std::string processImagePath(unsigned long pid) {
    if (pid == 0) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    std::vector<wchar_t> buf(32768);
    DWORD n = static_cast<DWORD>(buf.size());
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, buf.data(), &n)) out = utf8(buf.data(), static_cast<int>(n));
    CloseHandle(h);
    return out;
}


bool loopbackPortBindable(int port, std::string& error) {
    if (port <= 0 || port > 65535) return true;
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return true; // cannot check: let the real bind decide
    bool ok = true;
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        BOOL on = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
            const int e = WSAGetLastError();
            ok = false;
            error = e == WSAEADDRINUSE ? "address in use" : e == WSAEACCES ? "access denied (excluded port range?)"
                                                                           : "bind error " + std::to_string(e);
        }
        closesocket(s);
    }
    WSACleanup();
    return ok;
}

std::vector<TcpListener> tcpListeners(int port) {
    std::vector<TcpListener> out;
    if (port <= 0 || port > 65535) return out;
    const auto want = static_cast<u_short>(port);
    {
        const auto buf = readTable(AF_INET);
        if (!buf.empty()) {
            const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& row = t->table[i];
                if (ntohs(static_cast<u_short>(row.dwLocalPort)) != want) continue;
                TcpListener l;
                in_addr a{};
                a.S_un.S_addr = row.dwLocalAddr;
                char s[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &a, s, sizeof(s));
                l.address = s;
                l.pid = row.dwOwningPid;
                fillProcess(l);
                out.push_back(std::move(l));
            }
        }
    }
    {
        const auto buf = readTable(AF_INET6);
        if (!buf.empty()) {
            const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& row = t->table[i];
                if (ntohs(static_cast<u_short>(row.dwLocalPort)) != want) continue;
                TcpListener l;
                l.ipv6 = true;
                in6_addr a{};
                static_assert(sizeof(a.u.Byte) == sizeof(row.ucLocalAddr));
                for (size_t k = 0; k < sizeof(row.ucLocalAddr); ++k) a.u.Byte[k] = row.ucLocalAddr[k];
                char s[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET6, &a, s, sizeof(s));
                l.address = s;
                l.pid = row.dwOwningPid;
                fillProcess(l);
                out.push_back(std::move(l));
            }
        }
    }
    return out;
}

#elif defined(__APPLE__)

std::string processImagePath(unsigned long pid) {
    if (pid == 0) return {};
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    const int n = proc_pidpath(static_cast<int>(pid), buf, sizeof(buf));
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

bool loopbackPortBindable(int port, std::string& error) {
    if (port <= 0 || port > 65535) return true;
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return true; // cannot check: let the real bind decide
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    // No SO_REUSEADDR: POSIX never lets two sockets listen on one port with it unset.
    bool ok = true;
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        ok = false;
        error = errno == EADDRINUSE ? "address in use" : errno == EACCES ? "access denied" : std::strerror(errno);
    }
    close(s);
    return ok;
}

// Every process we may inspect (the user's own; others fail quietly) -> its TCP sockets in LISTEN on `port`.
std::vector<TcpListener> tcpListeners(int port) {
    std::vector<TcpListener> out;
    if (port <= 0 || port > 65535) return out;
    std::vector<pid_t> pids(4096);
    int bytes = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return out;
    pids.resize(static_cast<size_t>(bytes) / sizeof(pid_t));
    std::vector<proc_fdinfo> fds;
    for (const pid_t pid : pids) {
        if (pid <= 0) continue;
        const int need = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (need <= 0) continue;
        fds.resize(static_cast<size_t>(need) / sizeof(proc_fdinfo) + 16);
        const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
        if (got <= 0) continue;
        const size_t count = static_cast<size_t>(got) / sizeof(proc_fdinfo);
        for (size_t i = 0; i < count; ++i) {
            if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            socket_fdinfo si{};
            if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof(si)) != sizeof(si)) continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            const tcp_sockinfo& t = si.psi.soi_proto.pri_tcp;
            if (t.tcpsi_state != TSI_S_LISTEN) continue;
            const in_sockinfo& in = t.tcpsi_ini;
            if (ntohs(static_cast<uint16_t>(in.insi_lport)) != port) continue;
            TcpListener l;
            char addr[INET6_ADDRSTRLEN] = {};
            if (in.insi_vflag & INI_IPV4) {
                inet_ntop(AF_INET, &in.insi_laddr.ina_46.i46a_addr4, addr, sizeof(addr));
            } else {
                l.ipv6 = true;
                inet_ntop(AF_INET6, &in.insi_laddr.ina_6, addr, sizeof(addr));
            }
            l.address = addr;
            l.pid = static_cast<unsigned long>(pid);
            l.path = processImagePath(l.pid);
            const auto slash = l.path.find_last_of('/');
            l.image = slash == std::string::npos ? l.path : l.path.substr(slash + 1);
            out.push_back(std::move(l));
        }
    }
    return out;
}

#else

std::string processImagePath(unsigned long) { return {}; }
std::vector<TcpListener> tcpListeners(int) { return {}; }
bool loopbackPortBindable(int, std::string&) { return true; }

#endif

} // namespace ks::platform
