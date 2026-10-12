#include "samples/SamplePathRules.h"

#include "core/AppPaths.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace ks::samples {

namespace {

bool validUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + static_cast<size_t>(extra) >= s.size()) return false;
        for (int k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates, > U+10FFFF.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

bool reservedName(std::string_view seg) {
    // Base name before the first '.', case-insensitive. Windows also strips trailing spaces ("CON .txt").
    std::string base(seg.substr(0, seg.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const char* fixed[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (const char* f : fixed)
        if (base == f) return true;
    if (base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && base[3] >= '1' && base[3] <= '9')
        return true;
    // Superscript digits ¹²³ (UTF-8 C2 B9 / C2 B2 / C2 B3) are reserved too.
    if (base.size() == 5 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) &&
        static_cast<unsigned char>(base[3]) == 0xC2) {
        const auto d = static_cast<unsigned char>(base[4]);
        if (d == 0xB9 || d == 0xB2 || d == 0xB3) return true;
    }
    return false;
}

} // namespace

bool isValidLibraryId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    if (!(std::islower(static_cast<unsigned char>(id[0])) || std::isdigit(static_cast<unsigned char>(id[0])))) return false;
    for (char ch : id) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::islower(c) || std::isdigit(c) || c == '-')) return false;
    }
    return true;
}

bool isValidManifestPath(std::string_view p) {
    if (p.empty() || p.size() > 240 || p.front() == '/') return false;
    if (!validUtf8(p)) return false;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t slash = p.find('/', start);
        const std::string_view seg = p.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        for (char ch : seg) {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') return false;
            // Characters Windows refuses in file names.
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return false;
        }
        if (seg.back() == '.' || seg.back() == ' ') return false;
        if (reservedName(seg)) return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

std::optional<fs::path> cachePathFor(const fs::path& cacheRoot, std::string_view lib, std::string_view path) {
    if (!isValidLibraryId(lib) || !isValidManifestPath(path)) return std::nullopt;
    const fs::path libDir = (cacheRoot / pathFromUtf8(std::string(lib))).lexically_normal();
    const fs::path cand = (libDir / pathFromUtf8(std::string(path))).lexically_normal();
    if (!isPathInsideLexical(cand, libDir) || cand == libDir) return std::nullopt;
    // Canonical check (junctions/symlinks inside the cache must not lead outside it).
    if (!isPathInside(cand, libDir)) return std::nullopt;
    return cand;
}

std::optional<std::pair<std::string, std::string>> splitEntryId(std::string_view id) {
    const size_t slash = id.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    std::string lib(id.substr(0, slash)), rest(id.substr(slash + 1));
    if (!isValidLibraryId(lib) || !isValidManifestPath(rest)) return std::nullopt;
    return std::make_pair(std::move(lib), std::move(rest));
}

} // namespace ks::samples
