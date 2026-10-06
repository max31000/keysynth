// Bitcrusher: bit-depth reduction + sample-rate reduction (hold) with optional TPDF dither and dry/wet mix.
// C ABI plugin example (sdk/include/keysynth/plugin_abi.h, docs/PLUGINS.md). Build:
//   scripts/build_plugin.ps1 cpp_bitcrusher      -> plugins/.build/cpp_bitcrusher-<hash>.dll (hot-reloaded)
//
// Real-time rules apply to process/set_param/reset: no allocation, locks, IO or exceptions.

#include <keysynth/plugin_abi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

enum Param : uint32_t { Bits, Downsample, Dither, Mix, OutputDb, Clip, NumParams };

const char* const kDitherChoices[] = {"Off", "TPDF"};

const ks_param_spec kParams[NumParams] = {
    {sizeof(ks_param_spec), "bits", "Bits", "Crush", "bit", 1.0f, 16.0f, 8.0f, KS_SCALE_LINEAR, 0.0f, 0, nullptr, 0},
    {sizeof(ks_param_spec), "downsample", "Downsample", "Crush", "x", 1.0f, 64.0f, 1.0f, KS_SCALE_INT, 0.0f, 0,
     nullptr, 0},
    {sizeof(ks_param_spec), "dither", "Dither", "Crush", "", 0.0f, 1.0f, 0.0f, KS_SCALE_ENUM, 0.0f, 0, kDitherChoices,
     2},
    {sizeof(ks_param_spec), "mix", "Mix", "Output", "", 0.0f, 1.0f, 1.0f, KS_SCALE_LINEAR, 0.0f, 0, nullptr, 0},
    {sizeof(ks_param_spec), "output_db", "Output", "Output", "dB", -24.0f, 12.0f, 0.0f, KS_SCALE_LINEAR, 0.0f, 0,
     nullptr, 0},
    // Read-only readout: fraction of samples clipped in the last block.
    {sizeof(ks_param_spec), "clip", "Clip", "Output", "", 0.0f, 1.0f, 0.0f, KS_SCALE_LINEAR, 0.0f, KS_PARAM_READ_ONLY,
     nullptr, 0},
};

struct Crusher {
    float p[NumParams] = {8.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float held[2] = {0.0f, 0.0f};
    int counter = 0;
    uint32_t seed = 0x12345678u; // non-param state (save/load_state demo)
    float mixSmooth = 1.0f, gainSmooth = 1.0f;

    float rnd() noexcept { // xorshift32 -> [-0.5, 0.5)
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed) * (1.0f / 4294967296.0f) - 0.5f;
    }
};

ks_instance create() { return new (std::nothrow) Crusher(); }
void destroy(ks_instance i) { delete static_cast<Crusher*>(i); }
void prepare(ks_instance i, double, int32_t) { static_cast<Crusher*>(i)->counter = 0; }
void reset(ks_instance i) {
    auto* c = static_cast<Crusher*>(i);
    c->held[0] = c->held[1] = 0.0f;
    c->counter = 0;
}
void setParam(ks_instance i, uint32_t index, float value) {
    if (index < NumParams) static_cast<Crusher*>(i)->p[index] = value;
}
float getParam(ks_instance i, uint32_t index) { return index < NumParams ? static_cast<Crusher*>(i)->p[index] : 0.0f; }

void process(ks_instance inst, float** stereo, int32_t n, const ks_event*, int32_t) {
    auto* c = static_cast<Crusher*>(inst);
    const float levels = std::exp2(std::clamp(c->p[Bits], 1.0f, 16.0f) - 1.0f); // steps per unit amplitude
    const int hold = std::clamp(static_cast<int>(c->p[Downsample]), 1, 64);
    const bool dither = c->p[Dither] >= 0.5f;
    const float mixT = std::clamp(c->p[Mix], 0.0f, 1.0f);
    const float gainT = std::pow(10.0f, c->p[OutputDb] / 20.0f);
    int clipped = 0;
    for (int32_t s = 0; s < n; ++s) {
        c->mixSmooth += (mixT - c->mixSmooth) * 0.002f;
        c->gainSmooth += (gainT - c->gainSmooth) * 0.002f;
        if (c->counter == 0) {
            for (int ch = 0; ch < 2; ++ch) {
                float x = stereo[ch][s];
                if (dither) x += (c->rnd() + c->rnd()) / levels;
                if (x > 1.0f || x < -1.0f) ++clipped;
                x = std::clamp(x, -1.0f, 1.0f);
                c->held[ch] = std::round(x * levels) / levels;
            }
        }
        if (++c->counter >= hold) c->counter = 0;
        for (int ch = 0; ch < 2; ++ch) {
            const float dry = stereo[ch][s];
            stereo[ch][s] = (dry + (c->held[ch] - dry) * c->mixSmooth) * c->gainSmooth;
        }
    }
    c->p[Clip] = n > 0 ? static_cast<float>(clipped) / static_cast<float>(2 * n) : 0.0f;
}

int32_t saveState(ks_instance i, void* buf, int32_t cap) {
    const auto* c = static_cast<const Crusher*>(i);
    if (buf && cap >= static_cast<int32_t>(sizeof c->seed)) std::memcpy(buf, &c->seed, sizeof c->seed);
    return static_cast<int32_t>(sizeof c->seed);
}

int32_t loadState(ks_instance i, const void* data, int32_t size) {
    if (!data || size != static_cast<int32_t>(sizeof(uint32_t))) return 1;
    auto* c = static_cast<Crusher*>(i);
    std::memcpy(&c->seed, data, sizeof c->seed);
    if (c->seed == 0) c->seed = 1; // xorshift must not be 0
    return 0;
}

const ks_plugin_descriptor kDescriptor = {
    sizeof(ks_plugin_descriptor),
    KS_PLUGIN_ABI_VERSION,
    "cpp_bitcrusher",
    "Bitcrusher",
    "Distortion",
    KS_KIND_EFFECT,
    kParams,
    NumParams,
    &create,
    &destroy,
    &prepare,
    &reset,
    &process,
    &setParam,
    &getParam,
    nullptr, // tail_samples
    nullptr, // latency_samples
    &saveState,
    &loadState,
};

} // namespace

extern "C" KS_PLUGIN_EXPORT const ks_plugin_descriptor* ks_get_plugin(void) { return &kDescriptor; }
