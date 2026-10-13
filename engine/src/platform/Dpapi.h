#pragma once
// Secrets at rest (device token in cloud.json, cloud-api spec §6.2), bound to the current OS user.
// Windows: DPAPI (CryptProtectData, CurrentUser scope). macOS: AES-256-CBC + HMAC-SHA256 with a random key kept in
// the user's login keychain (one item per entropy; see Dpapi.cpp). Other platforms: not available (protect()
// returns nullopt, callers stay unlinked).

#include <optional>
#include <string>

namespace ks::platform {

// Encrypt `plain` for the current OS user, bound to `entropy`. nullopt on failure / unsupported platform.
std::optional<std::string> dpapiProtect(const std::string& plain, const std::string& entropy);
// Decrypt; nullopt when the blob belongs to another user/machine, the entropy differs, or it is corrupt.
std::optional<std::string> dpapiUnprotect(const std::string& blob, const std::string& entropy);

// %COMPUTERNAME% / the macOS computer name (or "keysynth-pc").
std::string computerName();

} // namespace ks::platform
