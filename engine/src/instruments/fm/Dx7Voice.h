#pragma once
// DX7 voice data and SysEx (.syx) parsing/encoding, written from the published DX7 MIDI data format.
// Control-thread code (allocates, throws nothing; errors are reported through return values).
//
// Unpacked voice ("VCED", 155 bytes + 1 pad): 6 operator blocks of 21 bytes in the order OP6..OP1, then
// pitch EG, algorithm, feedback, LFO, transpose and a 10-char name. Packed voice ("VMEM", 128 bytes) is the
// format inside a 32-voice bulk dump.

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ks::fm {

// Byte offsets inside an unpacked voice.
namespace vced {
inline constexpr int kOpSize = 21;
// Per-operator fields (add opBase(n)).
enum OpField {
    R1 = 0, R2, R3, R4, L1, L2, L3, L4, BreakPoint, LeftDepth, RightDepth, LeftCurve, RightCurve,
    RateScale, Ams, VelSens, OutLevel, Mode, Coarse, Fine, Detune
};
inline constexpr int PR1 = 126, PL1 = 130, Algorithm = 134, Feedback = 135, OscSync = 136, LfoSpeed = 137,
                     LfoDelay = 138, LfoPmd = 139, LfoAmd = 140, LfoSync = 141, LfoWave = 142, PitchModSens = 143,
                     Transpose = 144, Name = 145;
inline constexpr int kSize = 155;
// Operator n = 1..6 (DX7 numbering) -> offset of its block (OP6 comes first).
constexpr int opBase(int n) noexcept { return (6 - n) * kOpSize; }
} // namespace vced

struct Dx7Voice {
    // 156 bytes: the 155 VCED bytes + one pad byte (msfa reads 156-byte patches).
    std::array<uint8_t, 156> data{};

    std::string name() const;
    void setName(const std::string& n);
    // Clamp every field into its DX7 range (garbage in a .syx must not reach the engine).
    void sanitize() noexcept;

    static Dx7Voice initVoice(); // DX7 "INIT VOICE": OP1 carrier sine, alg 1
    static Dx7Voice fromPacked(std::span<const uint8_t, 128> p);
    void toPacked(std::span<uint8_t, 128> p) const;
};

// DX7 checksum over a data block: two's complement of the 7-bit sum.
uint8_t dx7Checksum(std::span<const uint8_t> data) noexcept;

struct SyxParseResult {
    std::vector<Dx7Voice> voices;
    std::string error;   // empty on success
    bool checksumOk = true; // false: loaded anyway (many carts in the wild have bad checksums)
};

// Accepts a 32-voice bulk dump (4104 bytes, F0 43 0n 09 20 00 ... F7), a single voice dump (163 bytes,
// F0 43 0n 00 01 1B ... F7) or a raw headerless 4096-byte bank. Several concatenated messages are accepted.
SyxParseResult parseSyx(std::span<const uint8_t> bytes);
SyxParseResult loadSyxFile(const std::filesystem::path& path);

// Encoders (used by tests and to export banks).
std::vector<uint8_t> encodeBulk(std::span<const Dx7Voice> voices, int channel = 0); // pads to 32 with INIT VOICE
std::vector<uint8_t> encodeSingle(const Dx7Voice& v, int channel = 0);

} // namespace ks::fm
