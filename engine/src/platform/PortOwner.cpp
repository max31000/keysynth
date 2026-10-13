#include "platform/PortOwner.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
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

#else

std::string processImagePath(unsigned long) { return {}; }
std::vector<TcpListener> tcpListeners(int) { return {}; }
bool loopbackPortBindable(int, std::string&) { return true; }

#endif

} // namespace ks::platform
