#pragma once
// `fm`: Yamaha DX7-compatible 6-operator FM synth on the vendored MSFA core (third_party/msfa, from Dexed).
//
// - Every DX7 voice parameter is a ParamSpec (ids `alg`, `feedback`, `op1_level`, `op1_eg_rate1`, ...); the
//   module builds the 156-byte DX7 voice from them whenever the ParamSet version changes.
// - Macros on top (non-DX7, applied as offsets while building the voice): brightness (modulator output
//   levels), attack/release (EG rate 1 / rate 4 of all operators), tune, voices (polyphony), volume.
// - Engine models: Modern (MSFA), Mark I and OPL (Dexed's EngineMkI / EngineOpl).
// - Controllers from ChannelState: pitch bend (range param), mod wheel and aftertouch routed DX7-style
//   (range + pitch/amp/EG-bias assign), sustain/sostenuto through VoiceAllocator.
// - State `{ "syx": "<path>", "voice": n }` loads voice n of a .syx bank/single dump and writes it into the
//   ParamSet (loadState, control thread). GraphBuilder applies the patch params *before* loadState, so the syx
//   voice wins on every rebuild; once the control side has copied the voice into the patch params it should set
//   `"applied": true` in the state, which makes loadState skip the syx (keeps later edits). Path must be inside
//   the allow-listed roots (ARCHITECTURE §11).
//
// MSFA renders in fixed 64-sample chunks; the module renders a chunk ahead and buffers the remainder, so
// latencySamples() == 0, but an event takes effect at the next chunk boundary (<= 63 samples late).
// MSFA keeps sample-rate-dependent lookup tables in globals: they are (re)initialized in prepare() only when the
// sample rate changes, which happens only while the device is stopped (ARCHITECTURE §5.4). Consequence: all fm
// instances in one process must run at the same sample rate.

#include "core/Module.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ks {

namespace fm {
struct Dx7Voice;
}

class FmSynth final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    FmSynth();
    ~FmSynth() override;

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;
    int activeVoices() const override { return activeVoices_.load(std::memory_order_relaxed); }

    nlohmann::json saveState() const override { return state_; }
    void loadState(const nlohmann::json& state) override;
    // Last loadState problem (empty = ok). Control thread.
    const std::string& stateError() const noexcept { return stateError_; }

    static constexpr int kMaxVoices = 32;

    // Param indices (order of moduleInfo().params).
    enum P {
        VolumeDb, Voices, EngineModel, VoiceModeP, Brightness, Attack, Release, Tune, PbRange,
        MwRange, MwPitch, MwAmp, MwEg, AtRange, AtPitch, AtAmp, AtEg,
        Alg, Feedback, OscSync, Transpose, LfoSpeed, LfoDelay, LfoPmd, LfoAmd, LfoSync, LfoWave, PitchModSens,
        PegRate1, PegRate2, PegRate3, PegRate4, PegLevel1, PegLevel2, PegLevel3, PegLevel4,
        OpBase
    };
    // Per-operator fields: index = OpBase + (op - 1) * kOpFields + field. Fields 0..20 follow the DX7 VCED
    // operator layout (detune stored as -7..+7); field 21 is the operator on/off switch.
    enum OpP {
        OpRate1, OpRate2, OpRate3, OpRate4, OpLevel1, OpLevel2, OpLevel3, OpLevel4, OpBreak, OpLeftDepth,
        OpRightDepth, OpLeftCurve, OpRightCurve, OpRateScale, OpAms, OpVelSens, OpOutLevel, OpMode, OpCoarse,
        OpFine, OpDetune, OpOn, kOpFields
    };
    static constexpr int opParam(int op, int field) noexcept { return OpBase + (op - 1) * kOpFields + field; }
    static constexpr int kParamCount = OpBase + 6 * kOpFields;

    // DX7 voice <-> plain param values (DX7 params only; macros untouched). Control thread.
    static std::vector<std::pair<std::string, float>> voiceToParams(const fm::Dx7Voice& v);
    static fm::Dx7Voice paramsToVoice(const ParamSet& p); // raw DX7 values, no macros
    // Writes a DX7 voice into this module's ParamSet.
    void applyVoice(const fm::Dx7Voice& v);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    nlohmann::json state_ = nlohmann::json::object();
    std::string stateError_;
    std::atomic<int> activeVoices_{0};
};

} // namespace ks
