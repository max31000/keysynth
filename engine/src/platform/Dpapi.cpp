#include "platform/Dpapi.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#elif defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <Security/SecRandom.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <fcntl.h>
#include <pwd.h>
#include <string.h> // timingsafe_bcmp
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <filesystem>
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
#elif defined(__APPLE__)
namespace {

// Blob: "KSK1" | iv[16] | AES-256-CBC(PKCS7) ciphertext | HMAC-SHA256(magic | iv | ciphertext)[32]. Key: 64 random
// bytes per `entropy` in ~/Library/Application Support/keysynth/.keys/<entropy>.key (dir 0700, file 0600); bytes
// 0..31 encrypt, 32..63 authenticate. Same guarantee as DPAPI CurrentUser (only this OS user can decrypt; a
// different entropy selects a different key). Not the keychain: its items trust the creating binary's signature,
// which changes with every ad-hoc-signed release, so each update would prompt for the login password.
constexpr char kMagic[4] = {'K', 'S', 'K', '1'};
constexpr size_t kIv = kCCBlockSizeAES128, kMac = CC_SHA256_DIGEST_LENGTH, kKey = 64;

std::string keyDir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        const passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : nullptr;
    }
    return home && *home ? std::string(home) + "/Library/Application Support/keysynth/.keys" : std::string();
}

std::string keyFile(const std::string& entropy) {
    std::string name;
    for (unsigned char c : entropy) name += std::isalnum(c) || c == '-' || c == '_' ? static_cast<char>(c) : '_';
    const std::string dir = keyDir();
    return dir.empty() ? std::string() : dir + "/" + name + ".key";
}

std::optional<std::string> loadKey(const std::string& entropy) {
    const std::string path = keyFile(entropy);
    if (path.empty()) return std::nullopt;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    std::string key(kKey, '\0');
    const ssize_t n = ::read(fd, key.data(), kKey);
    ::close(fd);
    if (n != static_cast<ssize_t>(kKey)) return std::nullopt;
    return key;
}

std::optional<std::string> loadOrCreateKey(const std::string& entropy) {
    if (auto k = loadKey(entropy)) return k;
    const std::string dir = keyDir(), path = keyFile(entropy);
    if (dir.empty()) return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ::chmod(dir.c_str(), 0700);
    std::string key(kKey, '\0');
    if (SecRandomCopyBytes(kSecRandomDefault, kKey, key.data()) != errSecSuccess) return std::nullopt;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? loadKey(entropy) : std::nullopt; // another process won the race
    const bool ok = ::write(fd, key.data(), kKey) == static_cast<ssize_t>(kKey) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) {
        ::unlink(path.c_str());
        return std::nullopt;
    }
    return key;
}

std::string hmac(const std::string& key, const std::string& msg) {
    std::string mac(kMac, '\0');
    CCHmac(kCCHmacAlgSHA256, key.data() + 32, 32, msg.data(), msg.size(), mac.data());
    return mac;
}

} // namespace

std::optional<std::string> dpapiProtect(const std::string& plain, const std::string& entropy) {
    const auto key = loadOrCreateKey(entropy);
    if (!key) return std::nullopt;
    std::string iv(kIv, '\0');
    if (SecRandomCopyBytes(kSecRandomDefault, kIv, iv.data()) != errSecSuccess) return std::nullopt;
    std::string ct(plain.size() + kCCBlockSizeAES128, '\0');
    size_t n = 0;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key->data(), 32, iv.data(), plain.data(),
                plain.size(), ct.data(), ct.size(), &n) != kCCSuccess)
        return std::nullopt;
    ct.resize(n);
    std::string blob = std::string(kMagic, sizeof(kMagic)) + iv + ct;
    blob += hmac(*key, blob);
    return blob;
}

std::optional<std::string> dpapiUnprotect(const std::string& blob, const std::string& entropy) {
    if (blob.size() < sizeof(kMagic) + kIv + kCCBlockSizeAES128 + kMac) return std::nullopt;
    if (blob.compare(0, sizeof(kMagic), kMagic, sizeof(kMagic)) != 0) return std::nullopt;
    const auto key = loadKey(entropy); // never create one here: nothing could decrypt with it
    if (!key) return std::nullopt;
    const std::string body = blob.substr(0, blob.size() - kMac);
    const std::string mac = hmac(*key, body);
    // Constant-time compare (timingsafe_bcmp).
    if (timingsafe_bcmp(mac.data(), blob.data() + body.size(), kMac) != 0) return std::nullopt;
    const char* iv = body.data() + sizeof(kMagic);
    const char* ct = iv + kIv;
    const size_t ctLen = body.size() - sizeof(kMagic) - kIv;
    std::string plain(ctLen, '\0');
    size_t n = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key->data(), 32, iv, ct, ctLen, plain.data(),
                plain.size(), &n) != kCCSuccess)
        return std::nullopt;
    plain.resize(n);
    return plain;
}

std::string computerName() {
    // The user-visible name from System Settings ("Maksim's MacBook Pro"), not the DNS host name.
    if (CFStringRef name = SCDynamicStoreCopyComputerName(nullptr, nullptr)) {
        char buf[256];
        const bool ok = CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        if (ok && buf[0] != '\0') return buf;
    }
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0') return host;
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
