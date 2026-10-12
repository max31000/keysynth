#include "control/Trust.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

using nlohmann::json;

namespace ks {

const char* toString(Trust t) {
    switch (t) {
    case Trust::Web: return "web";
    case Trust::Remote: return "remote";
    default: return "local";
    }
}

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

std::optional<Trust> classifyOrigin(const std::string& origin, int wsPort, int httpPort, const OriginPolicy& policy) {
    if (origin.empty()) return Trust::Local;
    if (origin == "https://mvv42.ru") return Trust::Web; // exact: no www, port, path, trailing slash
    if (policy.devBuild && !policy.extraWebOrigin.empty() && origin == policy.extraWebOrigin) return Trust::Web;
    static const std::regex re(R"(^http://(localhost|127\.0\.0\.1):(\d{1,5})$)");
    std::smatch m;
    if (!std::regex_match(origin, m, re)) return std::nullopt; // incl. "null"
    const int port = std::stoi(m[2].str());
    if (port == wsPort || port == httpPort) return Trust::Local;
    if (port == 5173 && policy.devBuild) return Trust::Local;
    return std::nullopt;
}

bool hostAllowed(const std::string& hostIn, int port) {
    const std::string host = lower(hostIn);
    const std::string p = ":" + std::to_string(port);
    return host == "127.0.0.1" + p || host == "localhost" + p;
}

bool isLogicalPath(const std::string& p, const std::string& prefix) {
    if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix) != 0) return false;
    if (p.front() == '/') return false;
    size_t start = 0;
    while (true) {
        const size_t slash = p.find('/', start);
        const std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (seg.back() == '.' || seg.back() == ' ') return false; // Win32 strips these: "x." == "x"
        for (unsigned char c : seg)
            if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

namespace {

bool anyPrefix(const std::string& p, std::initializer_list<const char*> prefixes) {
    for (const char* x : prefixes)
        if (isLogicalPath(p, x)) return true;
    return false;
}

bool patternPathOk(const json& v) {
    if (!v.is_string()) return v.is_null();
    const std::string s = v.get<std::string>();
    return s.empty() || anyPrefix(s, {"presets/patterns/", "userdata/patterns/"});
}

// Module state keys that name files. Every one must be a logical path.
std::string checkSlot(const json& slot) {
    if (!slot.is_object()) return {};
    auto st = slot.find("state");
    if (st == slot.end() || !st->is_object()) return {};
    for (const auto& [k, v] : st->items()) {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        if (k == "sfz") {
            if (!s.empty() && !isLogicalPath(s, "assets/samples/")) return "sampler sfz must be under assets/samples/";
        } else if (k == "syx" || k == "path" || k == "file" || k == "sample" || k == "dir") {
            if (!s.empty() && !anyPrefix(s, {"presets/", "userdata/", "assets/"}))
                return "state." + k + " must be a logical path";
        }
    }
    return {};
}

std::string checkFxArray(const json& fx) {
    if (!fx.is_array()) return {};
    for (const auto& f : fx)
        if (auto e = checkSlot(f); !e.empty()) return e;
    return {};
}

std::string checkPatch(const json& p) {
    if (!p.is_object()) return {};
    if (auto ls = p.find("layers"); ls != p.end() && ls->is_array())
        for (const auto& l : *ls) {
            if (!l.is_object()) continue;
            if (auto i = l.find("instrument"); i != l.end())
                if (auto e = checkSlot(*i); !e.empty()) return e;
            if (auto f = l.find("fx"); f != l.end())
                if (auto e = checkFxArray(*f); !e.empty()) return e;
        }
    if (auto m = p.find("master"); m != p.end() && m->is_object())
        if (auto f = m->find("fx"); f != m->end())
            if (auto e = checkFxArray(*f); !e.empty()) return e;
    if (auto r = p.find("rhythm"); r != p.end() && r->is_object())
        if (auto pt = r->find("pattern"); pt != r->end() && !patternPathOk(*pt)) return "rhythm.pattern must be a logical path";
    return {};
}

} // namespace

std::string webRestriction(const json& msg) {
    static const std::set<std::string> open = {
        "hello",      "get_catalog",   "set_param",     "get_patch",     "list_presets",   "add_layer",
        "remove_layer", "set_zone",    "set_instrument", "add_fx",       "remove_fx",      "move_fx",
        "set_fx_bypass", "note",       "cc",            "panic",         "list_patterns",  "set_pattern",
        "rescan_midi", "list_devices", "set_audio_device", "open_audio_panel", "restart_audio", "list_plugins",
        "save_preset", "save_pattern", "cloud_link",    "cloud_unlink",  "sync_now",       "samples_list",
        "samples_fetch", "samples_cancel", "samples_delete"};
    const std::string type = msg.value("type", std::string());
    if (open.count(type)) return {};
    if (type == "load_preset") {
        const auto it = msg.find("path");
        if (it != msg.end() && it->is_string() && anyPrefix(it->get<std::string>(), {"presets/factory/", "userdata/presets/"}))
            return {};
        return "load_preset: path must be under presets/factory/ or userdata/presets/";
    }
    if (type == "get_pattern") {
        const auto it = msg.find("path");
        if (it == msg.end() || patternPathOk(*it)) return {};
        return "get_pattern: path must be under presets/patterns/ or userdata/patterns/";
    }
    if (type == "transport") {
        const auto it = msg.find("pattern");
        if (it == msg.end() || patternPathOk(*it)) return {};
        return "transport: pattern must be under presets/patterns/ or userdata/patterns/";
    }
    if (type == "set_patch") {
        const auto it = msg.find("patch");
        if (it == msg.end()) return {}; // the handler reports bad_request
        const std::string e = checkPatch(*it);
        return e.empty() ? std::string() : "set_patch: " + e;
    }
    return "'" + type + "' is not available to web sessions";
}

std::string remoteRestriction(const json& msg) {
    static const std::set<std::string> denied = {"cloud_link",       "cloud_unlink",   "set_audio_device",
                                                 "open_audio_panel", "samples_delete", "reload_plugin"};
    const std::string type = msg.value("type", std::string());
    if (denied.count(type)) return "'" + type + "' is not available to remote sessions";
    std::string why = webRestriction(msg);
    if (!why.empty() && why.find("is not available to web sessions") != std::string::npos)
        why = "'" + type + "' is not available to remote sessions";
    return why;
}

std::string trustRestriction(Trust t, const json& msg) {
    switch (t) {
    case Trust::Web: return webRestriction(msg);
    case Trust::Remote: return remoteRestriction(msg);
    default: return {};
    }
}

} // namespace ks
