#include "core/AppPaths.h"

#include <cstdlib>
#include <cwctype>
#include <system_error>

namespace fs = std::filesystem;

namespace ks {

namespace {
bool looksLikeRoot(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p / "presets" / "factory", ec) || fs::exists(p / "AGENTS.md", ec);
}

std::optional<fs::path> searchUp(fs::path start) {
    std::error_code ec;
    start = fs::weakly_canonical(start, ec);
    for (int i = 0; i < 12 && !start.empty(); ++i) {
        if (looksLikeRoot(start)) return start;
        auto parent = start.parent_path();
        if (parent == start) break;
        start = parent;
    }
    return std::nullopt;
}

fs::path normalized(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec) c = p.lexically_normal();
    return c;
}
} // namespace

AppPaths AppPaths::fromRoot(const fs::path& rootIn) {
    AppPaths a;
    a.root = normalized(rootIn);
    a.factoryPresets = a.root / "presets" / "factory";
    a.userdata = a.root / "userdata";
    a.userPresets = a.userdata / "presets";
    a.settingsFile = a.userdata / "settings.json";
    a.uiDist = a.root / "ui" / "dist";
    return a;
}

AppPaths AppPaths::discover(const fs::path& exeDir) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    if (const char* env = std::getenv("KS_ROOT"); env && *env) return fromRoot(env);
    if (!exeDir.empty())
        if (auto r = searchUp(exeDir)) return fromRoot(*r);
    std::error_code ec;
    if (auto r = searchUp(fs::current_path(ec))) return fromRoot(*r);
#if defined(KS_SOURCE_DIR)
    return fromRoot(KS_SOURCE_DIR);
#else
    return fromRoot(fs::current_path(ec));
#endif
}

std::vector<fs::path> AppPaths::allowedRoots() const {
    return {root / "presets", root / "userdata", root / "assets", root / "plugins"};
}

bool isPathInside(const fs::path& p, const fs::path& rootDir) {
    return isPathInsideLexical(normalized(p), normalized(rootDir));
}

bool isPathInsideLexical(const fs::path& pIn, const fs::path& rootIn) {
    const fs::path a = pIn.lexically_normal();
    const fs::path r = rootIn.lexically_normal();
    auto ai = a.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++ai) {
        if (ri->empty()) continue; // trailing separator
        if (ai == a.end()) return false;
#if defined(_WIN32)
        // Case-insensitive compare on Windows.
        std::wstring x = ai->wstring(), y = ri->wstring();
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i)
            if (towlower(x[i]) != towlower(y[i])) return false;
#else
        if (*ai != *ri) return false;
#endif
    }
    return true;
}

std::optional<fs::path> AppPaths::resolveAllowed(const std::string& s) const {
    if (s.empty() || s.find('\0') != std::string::npos) return std::nullopt;
    // Reject UNC / device paths before touching the filesystem (canonicalizing would contact SMB hosts).
    if (s.size() >= 2 && (s[0] == '/' || s[0] == '\\') && (s[1] == '/' || s[1] == '\\')) return std::nullopt;
    fs::path p = pathFromUtf8(s);
    if (p.has_root_name() && p.root_name() != root.root_name()) return std::nullopt;
    if (p.is_relative()) p = root / p;
    p = p.lexically_normal();
    bool lexicallyInside = false;
    for (const auto& r : allowedRoots()) lexicallyInside = lexicallyInside || isPathInsideLexical(p, r);
    if (!lexicallyInside) return std::nullopt;
    p = normalized(p); // resolves symlinks/junctions; re-checked below
    for (const auto& r : allowedRoots())
        if (isPathInside(p, r)) return p;
    return std::nullopt;
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string AppPaths::relativeToRoot(const fs::path& p) const {
    if (isPathInside(p, root)) return pathToUtf8(normalized(p).lexically_relative(root));
    return pathToUtf8(p);
}

} // namespace ks
