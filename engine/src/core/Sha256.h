#pragma once
// SHA-256 (FIPS 180-4), incremental. Used for sample verification and preset sync hashes (ARCHITECTURE §15).
// Not for the audio thread (no allocation in update(), but nobody needs it there).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ks {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t n);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<uint8_t, 32> digest(); // finalizes; call reset() to reuse
    std::string hexDigest();          // 64 lowercase hex chars

    static std::string hex(std::string_view data);
    // Hash of a whole file; nullopt if it cannot be read.
    static std::optional<std::string> hexOfFile(const std::filesystem::path& p);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

} // namespace ks
