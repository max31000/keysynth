#include "core/AppPaths.h"

#include <juce_core/juce_core.h>

#include <cctype>
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

std::string envVar(const char* name) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void setProfileDirs(AppPaths& a) {
    a.userPresets = a.userdata / "presets";
    a.userPatterns = a.userdata / "patterns";
}

void setDataDirs(AppPaths& a, const fs::path& dataRoot) {
    a.dataRoot = normalized(dataRoot);
    a.settingsFile = a.dataRoot / "settings.json";
    a.cloudFile = a.dataRoot / "cloud.json";
    a.logsDir = a.dataRoot / "logs";
    a.sampleCache = a.dataRoot / "samples";
}

// Relative path of `p` below `base` with '/' separators ("" for base itself).
std::string relBelow(const fs::path& p, const fs::path& base) {
    const fs::path r = p.lexically_relative(base);
    if (r.empty() || r == ".") return {};
    return pathToUtf8(r);
}
} // namespace

AppPaths AppPaths::fromRoot(const fs::path& rootIn) {
    AppPaths a;
    a.mode = Mode::Dev;
    a.root = normalized(rootIn);
    a.factoryPresets = a.root / "presets" / "factory";
    a.factoryPatterns = a.root / "presets" / "patterns";
    a.uiDist = a.root / "ui" / "dist";
    setDataDirs(a, a.root / "userdata");
    a.pluginsDir = a.root / "plugins";
    a.pluginBuildDir = a.pluginsDir / ".build";
    a.localProfile = a.dataRoot;
    a.userdata = a.localProfile;
    a.recordingsDir = a.localProfile / "recordings";
    setProfileDirs(a);
    return a;
}

AppPaths AppPaths::installedLayout(const fs::path& appDir, const fs::path& dataRoot) {
    AppPaths a;
    a.mode = Mode::Installed;
    a.root = normalized(appDir);
    a.factoryPresets = a.root / "presets" / "factory";
    a.factoryPatterns = a.root / "presets" / "patterns";
    a.uiDist = a.root / "ui" / "dist";
    setDataDirs(a, dataRoot);
    a.pluginsDir = a.dataRoot / "plugins";
    a.pluginBuildDir = a.dataRoot / "faust-cache";
    a.localProfile = a.dataRoot / "userdata";
    a.userdata = a.localProfile;
    a.recordingsDir = a.localProfile / "recordings";
    setProfileDirs(a);
    return a;
}

AppPaths AppPaths::discover(const fs::path& exeDirIn) {
    fs::path exeDir = exeDirIn;
    if (exeDir.empty()) {
        const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        exeDir = fs::path(exe.getFullPathName().toWideCharPointer());
    }
    const std::string dataOverride = envVar("KS_DATA_DIR");
    std::error_code ec;
    if (!exeDir.empty() && fs::is_regular_file(exeDir / "installed.flag", ec)) {
        fs::path data = pathFromUtf8(dataOverride);
        if (data.empty()) {
            const auto la = juce::File::getSpecialLocation(juce::File::windowsLocalAppData);
            data = fs::path(la.getFullPathName().toWideCharPointer()) / "keysynth";
        }
        return installedLayout(exeDir, data);
    }
    AppPaths a;
    if (const std::string env = envVar("KS_ROOT"); !env.empty()) a = fromRoot(pathFromUtf8(env));
    else if (auto r = exeDir.empty() ? std::nullopt : searchUp(exeDir)) a = fromRoot(*r);
    else if (auto c = searchUp(fs::current_path(ec))) a = fromRoot(*c);
    else {
#if defined(KS_SOURCE_DIR)
        a = fromRoot(KS_SOURCE_DIR);
#else
        a = fromRoot(fs::current_path(ec));
#endif
    }
    if (!dataOverride.empty()) {
        setDataDirs(a, pathFromUtf8(dataOverride));
        a.localProfile = a.dataRoot;
        a.userdata = a.localProfile;
        a.recordingsDir = a.localProfile / "recordings";
        setProfileDirs(a);
    }
    return a;
}

bool AppPaths::isValidAccountId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

fs::path AppPaths::accountProfileDir(const std::string& id) const { return localProfile / "accounts" / id; }

bool AppPaths::setProfile(const std::string& id) {
    if (!id.empty() && !isValidAccountId(id)) return false;
    accountId = id;
    userdata = id.empty() ? localProfile : accountProfileDir(id);
    setProfileDirs(*this);
    return true;
}

std::vector<fs::path> AppPaths::allowedRoots() const {
    std::vector<fs::path> r = {root / "presets", localProfile, root / "assets", pluginsDir, sampleCache};
    return r;
}

bool isPathInside(const fs::path& p, const fs::path& rootDir) {
    return isPathInsideLexical(normalized(p), normalized(rootDir));
}

bool isPathInsideLexical(const fs::path& pIn, const fs::path& rootIn) {
    const fs::path a = pIn.lexically_normal();
    const fs::path r = rootIn.lexically_normal();
    if (r.empty()) return false;
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
    // Drive-relative ("C:foo") and root-relative ("/foo" on Windows) paths are ambiguous: reject.
    if (p.has_root_name() != p.has_root_directory()) return std::nullopt;
    if (p.is_relative()) {
        auto it = p.begin();
        const fs::path first = *it;
        fs::path rest;
        for (++it; it != p.end(); ++it) rest /= *it;
        if (first == "userdata") p = userdata / rest;
        else if (first == "plugins") p = pluginsDir / rest;
        else p = root / p;
    }
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

std::string AppPaths::relativeToRoot(const fs::path& pIn) const {
    const fs::path p = normalized(pIn);
    auto join = [](const char* prefix, const std::string& rel) {
        return rel.empty() ? std::string(prefix) : std::string(prefix) + "/" + rel;
    };
    if (isPathInsideLexical(p, normalized(sampleCache))) return join("assets/samples", relBelow(p, normalized(sampleCache)));
    if (isPathInsideLexical(p, normalized(userdata))) return join("userdata", relBelow(p, normalized(userdata)));
    const fs::path r = normalized(root);
    if (isPathInsideLexical(p, r) && !isPathInsideLexical(p, normalized(localProfile))) return relBelow(p, r);
    if (isPathInsideLexical(p, normalized(pluginsDir))) return join("plugins", relBelow(p, normalized(pluginsDir)));
    return pathToUtf8(p);
}

} // namespace ks
