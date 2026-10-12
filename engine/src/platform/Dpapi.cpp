#include "platform/Dpapi.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#endif

#include <cstdlib>

namespace ks::platform {

#if defined(_WIN32)
namespace {
DATA_BLOB blobOf(const std::string& s) {
    DATA_BLOB b;
    b.cbData = static_cast<DWORD>(s.size());
    b.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(s.data()));
    return b;
}
} // namespace

std::optional<std::string> dpapiProtect(const std::string& plain, const std::string& entropy) {
    DATA_BLOB in = blobOf(plain), ent = blobOf(entropy), out{};
    if (!CryptProtectData(&in, L"keysynth", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    std::string r(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

std::optional<std::string> dpapiUnprotect(const std::string& blob, const std::string& entropy) {
    if (blob.empty()) return std::nullopt;
    DATA_BLOB in = blobOf(blob), ent = blobOf(entropy), out{};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    std::string r(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}

std::string computerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &n) && n > 0) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), len, nullptr, nullptr);
        return s;
    }
    return "keysynth-pc";
}
#else
std::optional<std::string> dpapiProtect(const std::string&, const std::string&) { return std::nullopt; }
std::optional<std::string> dpapiUnprotect(const std::string&, const std::string&) { return std::nullopt; }
std::string computerName() {
    const char* h = std::getenv("HOSTNAME");
    return h && *h ? std::string(h) : std::string("keysynth-pc");
}
#endif

} // namespace ks::platform
