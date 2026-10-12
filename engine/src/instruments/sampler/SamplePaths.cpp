#include "instruments/sampler/SamplePaths.h"

#include "core/AppPaths.h"

#include <cstdlib>
#include <mutex>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ks {

namespace {

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// `<main>` for a root like `<main>/.claude/worktrees/<name>`; empty otherwise.
fs::path mainCheckoutOf(const fs::path& root) {
    const fs::path r = root.lexically_normal();
    const fs::path wtDir = r.parent_path();
    if (wtDir.filename() == "worktrees" && wtDir.parent_path().filename() == ".claude")
        return wtDir.parent_path().parent_path();
    return {};
}

// "assets/samples/x.sfz" -> "samples/x.sfz"; nullopt if the path is not below assets/.
std::optional<fs::path> belowAssets(const fs::path& relToRoot) {
    auto it = relToRoot.begin();
    if (it == relToRoot.end() || *it != "assets") return std::nullopt;
    fs::path rest;
    for (++it; it != relToRoot.end(); ++it) rest /= *it;
    if (rest.empty()) return std::nullopt;
    return rest;
}

struct SampleHooks {
    std::mutex m;
    std::optional<fs::path> cacheDir;
    SampleFetchHook fetch;
};
SampleHooks& hooks() {
    static SampleHooks* h = new SampleHooks(); // leaked: loader threads may resolve during static destruction
    return *h;
}

} // namespace

void setSampleCacheDir(const fs::path& dir) {
    std::lock_guard<std::mutex> lk(hooks().m);
    hooks().cacheDir = dir;
}

fs::path sampleCacheDir() {
    {
        std::lock_guard<std::mutex> lk(hooks().m);
        if (hooks().cacheDir) return *hooks().cacheDir;
    }
    const fs::path d = AppPaths::discover().sampleCache;
    std::lock_guard<std::mutex> lk(hooks().m);
    if (!hooks().cacheDir) hooks().cacheDir = d;
    return *hooks().cacheDir;
}

void setSampleFetchHook(SampleFetchHook hook) {
    std::lock_guard<std::mutex> lk(hooks().m);
    hooks().fetch = std::move(hook);
}

SampleFetchState querySampleFetch(const std::string& entryId, bool fresh) {
    SampleFetchHook h;
    {
        std::lock_guard<std::mutex> lk(hooks().m);
        h = hooks().fetch;
    }
    return h ? h(entryId, fresh) : SampleFetchState::Unavailable;
}

std::optional<std::string> sampleEntryIdOf(const std::string& spec) {
    static const std::string prefix = "assets/samples/";
    if (spec.rfind(prefix, 0) != 0 || spec.size() <= prefix.size()) return std::nullopt;
    std::string id = spec.substr(prefix.size());
    if (id.find('\\') != std::string::npos || id.find("..") != std::string::npos || id.find('/') == std::string::npos)
        return std::nullopt;
    return id;
}

std::vector<fs::path> sampleAssetDirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    auto add = [&](const fs::path& d) {
        if (!d.empty() && fs::is_directory(d, ec)) dirs.push_back(d.lexically_normal());
    };
    add(root / "assets");
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    if (const char* env = std::getenv("KS_ASSETS_DIR"); env && *env) add(pathFromUtf8(env));
    if (const fs::path main = mainCheckoutOf(root); !main.empty()) add(main / "assets");
    return dirs;
}

SamplePathResult resolveSamplePath(const std::string& spec, const fs::path& root) {
    SamplePathResult res;
    if (spec.empty()) {
        res.error = "empty sfz path";
        return res;
    }
    const AppPaths paths = AppPaths::fromRoot(root);
    if (auto p = paths.resolveAllowed(spec); p && isFile(*p)) {
        res.path = *p;
        return res;
    }

    // Fallback for assets/...: other asset directories (env var, main checkout of a worktree).
    fs::path given = pathFromUtf8(spec).lexically_normal();
    fs::path rel = given.is_absolute() ? given.lexically_relative(paths.root) : given;
    if (auto rest = belowAssets(rel)) {
        for (const fs::path& dir : sampleAssetDirs(paths.root)) {
            const fs::path cand = (dir / *rest).lexically_normal();
            if (!isPathInside(cand, dir)) continue; // `..` escape
            if (isFile(cand)) {
                res.path = cand;
                return res;
            }
        }
        // assets/samples/<lib>/<path> -> <cache>/<lib>/<path>
        auto it = rest->begin();
        if (it != rest->end() && *it == "samples") {
            fs::path below;
            for (++it; it != rest->end(); ++it) below /= *it;
            const fs::path cache = sampleCacheDir().lexically_normal();
            const fs::path cand = (cache / below).lexically_normal();
            if (!below.empty() && isPathInside(cand, cache) && isFile(cand)) {
                res.path = cand;
                return res;
            }
        }
        res.error = "sfz not found: '" + spec + "' (searched repo assets/, $KS_ASSETS_DIR, main checkout assets/, sample cache)";
        return res;
    }
    res.error = paths.resolveAllowed(spec) ? "sfz not found: '" + spec + "'"
                                           : "sfz path outside the allowed roots: '" + spec + "'";
    return res;
}

} // namespace ks
