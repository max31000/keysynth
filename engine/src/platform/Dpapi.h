#pragma once
// Windows DPAPI (CryptProtectData, CurrentUser scope) for secrets at rest (device token in cloud.json,
// cloud-api spec §6.2). Other platforms: not available (protect() returns nullopt, callers stay unlinked).

#include <optional>
#include <string>

namespace ks::platform {

// Encrypt `plain` for the current Windows user, bound to `entropy`. nullopt on failure / unsupported platform.
std::optional<std::string> dpapiProtect(const std::string& plain, const std::string& entropy);
// Decrypt; nullopt when the blob belongs to another user/machine, the entropy differs, or it is corrupt.
std::optional<std::string> dpapiUnprotect(const std::string& blob, const std::string& entropy);

// %COMPUTERNAME% (or "keysynth-pc").
std::string computerName();

} // namespace ks::platform
