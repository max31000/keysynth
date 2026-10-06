#include "instruments/fm/Dx7Voice.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace ks::fm {

namespace {

constexpr int kBulkData = 4096;
constexpr int kSingleData = 155;

uint8_t clampByte(uint8_t v, uint8_t hi) noexcept { return v > hi ? hi : v; }

// Max value per op field (index = vced::OpField).
constexpr uint8_t kOpMax[vced::kOpSize] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3,
                                           7,  3,  7,  99, 1,  31, 99, 14};

} // namespace

std::string Dx7Voice::name() const {
    std::string s;
    for (int i = 0; i < 10; ++i) {
        const char c = static_cast<char>(data[static_cast<size_t>(vced::Name + i)]);
        s.push_back(c >= 32 && c < 127 ? c : ' ');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void Dx7Voice::setName(const std::string& n) {
    for (int i = 0; i < 10; ++i) {
        const char c = i < static_cast<int>(n.size()) ? n[static_cast<size_t>(i)] : ' ';
        data[static_cast<size_t>(vced::Name + i)] = static_cast<uint8_t>(c >= 32 && c < 127 ? c : ' ');
    }
}

void Dx7Voice::sanitize() noexcept {
    for (int op = 0; op < 6; ++op)
        for (int f = 0; f < vced::kOpSize; ++f) {
            auto& b = data[static_cast<size_t>(op * vced::kOpSize + f)];
            b = clampByte(b, kOpMax[f]);
        }
    for (int i = 0; i < 8; ++i) data[static_cast<size_t>(vced::PR1 + i)] = clampByte(data[static_cast<size_t>(vced::PR1 + i)], 99);
    auto clampAt = [&](int idx, uint8_t hi) { data[static_cast<size_t>(idx)] = clampByte(data[static_cast<size_t>(idx)], hi); };
    clampAt(vced::Algorithm, 31);
    clampAt(vced::Feedback, 7);
    clampAt(vced::OscSync, 1);
    clampAt(vced::LfoSpeed, 99);
    clampAt(vced::LfoDelay, 99);
    clampAt(vced::LfoPmd, 99);
    clampAt(vced::LfoAmd, 99);
    clampAt(vced::LfoSync, 1);
    clampAt(vced::LfoWave, 5);
    clampAt(vced::PitchModSens, 7);
    clampAt(vced::Transpose, 48);
    for (int i = 0; i < 10; ++i) {
        auto& c = data[static_cast<size_t>(vced::Name + i)];
        if (c < 32 || c > 126) c = ' ';
    }
    data[155] = 0x3f; // pad (Dexed uses it for the operator on/off mask; msfa ignores it)
}

Dx7Voice Dx7Voice::initVoice() {
    Dx7Voice v;
    for (int n = 1; n <= 6; ++n) {
        uint8_t* o = v.data.data() + vced::opBase(n);
        for (int i = 0; i < 4; ++i) o[vced::R1 + i] = 99;
        o[vced::L1] = o[vced::L2] = o[vced::L3] = 99;
        o[vced::L4] = 0;
        o[vced::BreakPoint] = 39;
        o[vced::OutLevel] = n == 1 ? 99 : 0;
        o[vced::Coarse] = 1;
        o[vced::Detune] = 7;
    }
    for (int i = 0; i < 4; ++i) {
        v.data[static_cast<size_t>(vced::PR1 + i)] = 99;
        v.data[static_cast<size_t>(vced::PL1 + i)] = 50;
    }
    v.data[vced::OscSync] = 1;
    v.data[vced::LfoSpeed] = 35;
    v.data[vced::LfoSync] = 1;
    v.data[vced::PitchModSens] = 3;
    v.data[vced::Transpose] = 24;
    v.setName("INIT VOICE");
    v.sanitize();
    return v;
}

Dx7Voice Dx7Voice::fromPacked(std::span<const uint8_t, 128> p) {
    Dx7Voice v;
    auto& d = v.data;
    for (int op = 0; op < 6; ++op) { // packed order is OP6..OP1 as well
        const uint8_t* s = p.data() + op * 17;
        uint8_t* o = d.data() + op * vced::kOpSize;
        for (int i = 0; i < 11; ++i) o[i] = s[i] & 0x7f;
        o[vced::LeftCurve] = s[11] & 3;
        o[vced::RightCurve] = (s[11] >> 2) & 3;
        o[vced::RateScale] = s[12] & 7;
        o[vced::Detune] = (s[12] >> 3) & 0x0f;
        o[vced::Ams] = s[13] & 3;
        o[vced::VelSens] = (s[13] >> 2) & 7;
        o[vced::OutLevel] = s[14] & 0x7f;
        o[vced::Mode] = s[15] & 1;
        o[vced::Coarse] = (s[15] >> 1) & 0x1f;
        o[vced::Fine] = s[16] & 0x7f;
    }
    for (int i = 0; i < 8; ++i) d[static_cast<size_t>(vced::PR1 + i)] = p[static_cast<size_t>(102 + i)] & 0x7f;
    d[vced::Algorithm] = p[110] & 0x1f;
    d[vced::Feedback] = p[111] & 7;
    d[vced::OscSync] = (p[111] >> 3) & 1;
    d[vced::LfoSpeed] = p[112] & 0x7f;
    d[vced::LfoDelay] = p[113] & 0x7f;
    d[vced::LfoPmd] = p[114] & 0x7f;
    d[vced::LfoAmd] = p[115] & 0x7f;
    d[vced::LfoSync] = p[116] & 1;
    d[vced::LfoWave] = (p[116] >> 1) & 7;
    d[vced::PitchModSens] = (p[116] >> 4) & 7;
    d[vced::Transpose] = p[117] & 0x7f;
    for (int i = 0; i < 10; ++i) d[static_cast<size_t>(vced::Name + i)] = p[static_cast<size_t>(118 + i)] & 0x7f;
    v.sanitize();
    return v;
}

void Dx7Voice::toPacked(std::span<uint8_t, 128> p) const {
    std::fill(p.begin(), p.end(), uint8_t{0});
    const auto& d = data;
    for (int op = 0; op < 6; ++op) {
        uint8_t* s = p.data() + op * 17;
        const uint8_t* o = d.data() + op * vced::kOpSize;
        for (int i = 0; i < 11; ++i) s[i] = o[i] & 0x7f;
        s[11] = static_cast<uint8_t>((o[vced::LeftCurve] & 3) | ((o[vced::RightCurve] & 3) << 2));
        s[12] = static_cast<uint8_t>((o[vced::RateScale] & 7) | ((o[vced::Detune] & 0x0f) << 3));
        s[13] = static_cast<uint8_t>((o[vced::Ams] & 3) | ((o[vced::VelSens] & 7) << 2));
        s[14] = o[vced::OutLevel] & 0x7f;
        s[15] = static_cast<uint8_t>((o[vced::Mode] & 1) | ((o[vced::Coarse] & 0x1f) << 1));
        s[16] = o[vced::Fine] & 0x7f;
    }
    for (int i = 0; i < 8; ++i) p[static_cast<size_t>(102 + i)] = d[static_cast<size_t>(vced::PR1 + i)];
    p[110] = d[vced::Algorithm] & 0x1f;
    p[111] = static_cast<uint8_t>((d[vced::Feedback] & 7) | ((d[vced::OscSync] & 1) << 3));
    p[112] = d[vced::LfoSpeed];
    p[113] = d[vced::LfoDelay];
    p[114] = d[vced::LfoPmd];
    p[115] = d[vced::LfoAmd];
    p[116] = static_cast<uint8_t>((d[vced::LfoSync] & 1) | ((d[vced::LfoWave] & 7) << 1) |
                                  ((d[vced::PitchModSens] & 7) << 4));
    p[117] = d[vced::Transpose];
    for (int i = 0; i < 10; ++i) p[static_cast<size_t>(118 + i)] = d[static_cast<size_t>(vced::Name + i)] & 0x7f;
}

uint8_t dx7Checksum(std::span<const uint8_t> data) noexcept {
    unsigned sum = 0;
    for (uint8_t b : data) sum += b;
    return static_cast<uint8_t>((128u - (sum & 0x7fu)) & 0x7fu);
}

SyxParseResult parseSyx(std::span<const uint8_t> bytes) {
    SyxParseResult r;
    // Raw headerless bank (some archives store just the 4096 data bytes).
    if (bytes.size() == static_cast<size_t>(kBulkData) && bytes[0] != 0xf0) {
        for (int i = 0; i < 32; ++i)
            r.voices.push_back(Dx7Voice::fromPacked(std::span<const uint8_t, 128>(bytes.data() + i * 128, 128)));
        return r;
    }
    size_t pos = 0;
    while (pos < bytes.size()) {
        if (bytes[pos] != 0xf0) {
            ++pos; // skip garbage between messages
            continue;
        }
        const size_t remain = bytes.size() - pos;
        if (remain < 6 || bytes[pos + 1] != 0x43 || (bytes[pos + 2] & 0xf0) != 0) {
            r.error = "not a Yamaha SysEx message";
            break;
        }
        const uint8_t format = bytes[pos + 3];
        const int count = (bytes[pos + 4] << 7) | bytes[pos + 5];
        const uint8_t* body = bytes.data() + pos + 6;
        if (format == 0x09 && count == kBulkData && remain >= static_cast<size_t>(kBulkData + 8)) {
            const std::span<const uint8_t> dataSpan(body, static_cast<size_t>(kBulkData));
            if (dx7Checksum(dataSpan) != (body[kBulkData] & 0x7f)) r.checksumOk = false;
            for (int i = 0; i < 32; ++i)
                r.voices.push_back(Dx7Voice::fromPacked(std::span<const uint8_t, 128>(body + i * 128, 128)));
            pos += static_cast<size_t>(kBulkData + 8);
        } else if (format == 0x00 && count == kSingleData && remain >= static_cast<size_t>(kSingleData + 8)) {
            const std::span<const uint8_t> dataSpan(body, static_cast<size_t>(kSingleData));
            if (dx7Checksum(dataSpan) != (body[kSingleData] & 0x7f)) r.checksumOk = false;
            Dx7Voice v;
            std::copy(body, body + kSingleData, v.data.begin());
            v.sanitize();
            r.voices.push_back(v);
            pos += static_cast<size_t>(kSingleData + 8);
        } else {
            r.error = "unsupported or truncated DX7 SysEx (format " + std::to_string(format) + ", " +
                      std::to_string(count) + " bytes)";
            break;
        }
    }
    if (r.voices.empty() && r.error.empty()) r.error = "no DX7 voice data found";
    return r;
}

SyxParseResult loadSyxFile(const std::filesystem::path& path) {
    SyxParseResult r;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        r.error = "cannot open " + path.generic_string();
        return r;
    }
    if (size > (1u << 20)) {
        r.error = "file too large for a DX7 bank";
        return r;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r.error = "cannot open " + path.generic_string();
        return r;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseSyx(bytes);
}

std::vector<uint8_t> encodeBulk(std::span<const Dx7Voice> voices, int channel) {
    std::vector<uint8_t> out = {0xf0, 0x43, static_cast<uint8_t>(channel & 0x0f), 0x09, 0x20, 0x00};
    std::array<uint8_t, 128> packed{};
    const Dx7Voice init = Dx7Voice::initVoice();
    for (size_t i = 0; i < 32; ++i) {
        (i < voices.size() ? voices[i] : init).toPacked(packed);
        out.insert(out.end(), packed.begin(), packed.end());
    }
    out.push_back(dx7Checksum(std::span<const uint8_t>(out.data() + 6, static_cast<size_t>(kBulkData))));
    out.push_back(0xf7);
    return out;
}

std::vector<uint8_t> encodeSingle(const Dx7Voice& v, int channel) {
    std::vector<uint8_t> out = {0xf0, 0x43, static_cast<uint8_t>(channel & 0x0f), 0x00, 0x01, 0x1b};
    out.insert(out.end(), v.data.begin(), v.data.begin() + kSingleData);
    out.push_back(dx7Checksum(std::span<const uint8_t>(out.data() + 6, static_cast<size_t>(kSingleData))));
    out.push_back(0xf7);
    return out;
}

} // namespace ks::fm
