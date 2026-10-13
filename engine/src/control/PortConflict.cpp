#include "control/PortConflict.h"

#include <algorithm>
#include <cctype>

namespace ks {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string who(const PortConflict& c, bool ru) {
    const std::string pid = " (PID " + std::to_string(c.pid) + ")";
    if (c.kind == PortOwnerKind::Keysynth) return (ru ? "другим движком keysynth" : "another keysynth engine") + pid;
    if (c.name.empty()) return (ru ? "неизвестным процессом" : "an unknown process") + pid;
    return (ru ? "процессом " : "") + c.name + pid;
}
} // namespace

bool blocksLoopback(const platform::TcpListener& l) {
    return l.address == "127.0.0.1" || l.address == "0.0.0.0" || l.address == "::";
}

bool isKeysynthImage(const std::string& image) {
    const std::string l = lower(image);
    return l == "keysynth-engine.exe" || l == "keysynth-engine"; // Windows / macOS
}

std::optional<PortConflict> findConflict(int port, const std::vector<platform::TcpListener>& listeners) {
    std::optional<PortConflict> best;
    for (const auto& l : listeners) {
        if (!blocksLoopback(l)) continue;
        PortConflict c;
        c.port = port;
        c.kind = isKeysynthImage(l.image) ? PortOwnerKind::Keysynth : PortOwnerKind::Foreign;
        c.pid = l.pid;
        c.name = l.image;
        c.path = l.path;
        c.address = l.address;
        if (!best || (c.kind == PortOwnerKind::Keysynth && best->kind != PortOwnerKind::Keysynth)) best = std::move(c);
    }
    return best;
}

std::vector<platform::TcpListener> shadowListeners(const std::vector<platform::TcpListener>& listeners) {
    std::vector<platform::TcpListener> out;
    for (const auto& l : listeners)
        if (!blocksLoopback(l)) out.push_back(l);
    return out;
}

std::string conflictMessage(const PortConflict& c, bool ru) {
    const std::string port = std::to_string(c.port);
    if (c.kind == PortOwnerKind::Unknown) {
        const std::string err = c.error.empty() ? std::string() : " (" + c.error + ")";
        return ru ? "Порт " + port + " недоступен" + err + ". keysynth повторяет попытку каждые 3 с."
                  : "Port " + port + " is unavailable" + err + ". keysynth retries every 3 s.";
    }
    return ru ? "Порт " + port + " занят " + who(c, true) + ". Закройте его — keysynth переподключится автоматически."
              : "Port " + port + " is in use by " + who(c, false) +
                    ". Close it and keysynth will reconnect automatically.";
}

std::string portFreeMessage(int port, bool ru) {
    const std::string p = std::to_string(port);
    return ru ? "Порт " + p + " свободен — движок снова доступен." : "Port " + p + " is free — the engine is reachable again.";
}

std::string conflictLogLine(const PortConflict& c) {
    const char* kind = c.kind == PortOwnerKind::Keysynth ? "keysynth" : c.kind == PortOwnerKind::Foreign ? "foreign" : "unknown";
    std::string s = "port " + std::to_string(c.port) + " busy (" + kind + ")";
    if (c.kind != PortOwnerKind::Unknown) {
        s += ": " + (c.name.empty() ? std::string("?") : c.name) + " (PID " + std::to_string(c.pid);
        if (!c.path.empty()) s += ", " + c.path;
        s += ") on " + c.address;
    }
    if (!c.error.empty()) s += ": " + c.error;
    return s;
}

} // namespace ks
