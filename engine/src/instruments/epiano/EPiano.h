#pragma once
// `epiano`: physically-informed electric pianos, no samples (ARCHITECTURE §7).
//
// Per voice (modal synthesis, dsp/ModalBank, 8 modes):
//   hammer = alpha-shaped force pulse (contact time from model, register, hardness, velocity)
//   -> tine / reed modes: two fundamental normal modes of the coupled tine+tonebar pair (fast "tine" mode,
//      long "tonebar" mode, slightly split -> slow beating), clamped-free beam overtones excited according to the
//      strike position (dsp/BeamModes), a second tine polarization (beating bell), one mechanical thump mode
//   -> pickup nonlinearity on the tip displacement:
//        Rhodes: electromagnetic, v ~ d/dt Phi(u), Phi = 1/(1+u^2), u = (x + offset)/width  (bark, octave when
//                centred; tine buzz = soft contact limit towards the pickup)
//        Wurlitzer: electrostatic, v ~ d/dt C(x), C = x/(1 - a x) (asymmetric -> even harmonics)
//   + mechanical noise (hammer click, damper thump)
// Damper: on key release the felt engages over ~8 ms, raising every mode's decay rate; the sustain pedal is read
// as a continuous CC64 value (half-damper: partial engagement). Re-striking a ringing note reuses its voice and
// adds the new hammer impulse to the existing vibration. The VoiceAllocator's own sustain is disabled, so
// pedal-held notes count as "released" for stealing (oldest released first) - accepted trade-off. Stolen voices hand their state to a small "ghost" pool
// that fades them out over 6 ms while the new note starts (no clicks).
// Module chain (mono until the end): sum voices -> preamp (asymmetric soft saturation) -> DC block -> tone /
// presence / bass EQ (model-specific) -> Suitcase stereo vibrato (Suitcase model only) -> volume.

#include "core/Module.h"
#include "core/VoiceAllocator.h"
#include "dsp/ModalBank.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ks {

class EPiano final : public Module {
public:
    static const ModuleInfo& moduleInfo();
    EPiano();

    void prepare(double sampleRate, int maxBlock) override;
    void reset() override;
    void process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) override;
    int tailSamples() const override;
    int activeVoices() const override { return activeVoices_.load(std::memory_order_relaxed); }

    // Param indices (order of moduleInfo().params).
    enum P {
        Model, Tone, Bark, HammerHardness, VelocityCurve, Dynamics, Decay, Release, TineMix, StrikePosition,
        PickupPosition, PickupDistance, Drive, Noise, KeyVariation, StereoDepth, StereoRate, Polyphony, VolumeDb,
        Count
    };
    enum class ModelId { MkI = 0, MkII = 1, Suitcase = 2, Wurlitzer = 3, PianoBass = 4 };

    static constexpr int kMaxVoices = 32;
    static constexpr int kModes = 8;
    static constexpr int kGhosts = 12;

    // Deliberate tuning offset (cents) of a key: mild stretch tuning + per-key variation (scaled by the
    // key_variation param). Measured pitch = 12-TET + this (tests rely on it).
    static float tuningCents(int note, float keyVariation) noexcept;
    // Deterministic per-key pseudo-random value in [-1, 1].
    static float keyRandom(int note, int salt) noexcept;

    struct ModelDef;

    // Block-rate values shared by all voices.
    struct Shared {
        double sampleRate = 48000.0;
        const ModelDef* model = nullptr;
        ModelId modelId = ModelId::MkI;
        float tone = 0.5f, bark = 0.5f, hardness = 0.5f, velGamma = 1.0f, dynamicsDb = 30.0f;
        float decayMul = 1.0f, releaseMul = 1.0f, tineMix = 0.5f, strike = 0.5f;
        float pickupPos = 0.5f, pickupDist = 0.5f, noise = 0.3f, keyVar = 0.5f;
        float bendSemis = 0.0f;
        float pedalLift = 0.0f; // 0 = dampers on strings, 1 = fully lifted
        float engageCoef = 0.1f; // damper felt engagement smoothing per control chunk
        EPiano* owner = nullptr;
    };

    struct Voice {
        Shared* sh = nullptr;
        dsp::ModalBank<kModes> modes;
        std::array<float, kModes> rateFree{}, rateDamp{}, baseOmega{};
        int note = -1;
        int index = 0;
        float f0 = 261.6f;
        float velocity = 0.0f;
        bool active = false;
        bool held = false;
        bool forceDamp = false;
        bool damperWasOn = false;
        float engage = 0.0f, engageApplied = -1.0f, bendApplied = 0.0f;
        int ctlCountdown = 0; // samples until the next control update
        // hammer pulse
        int pulsePos = 0, pulseLen = 0;
        float pulseEnv = 0.0f, pulseDecay = 0.0f, pulseInvTau = 0.0f; // alpha pulse (n/tau) e^{1 - n/tau}
        // pickup
        bool electrostatic = false;
        float invW = 1.0f, u0 = 0.0f, uBuzz = 10.0f, nlA = 0.0f, pickupNorm = 1.0f, prevPhi = 0.0f;
        float outGain = 0.0f;
        float silenceScale = 1.0f; // mode energy -> output power (silence detection)
        // noise
        uint32_t rng = 1;
        float clickEnv = 0.0f, clickDecay = 0.0f, clickLevel = 0.0f, noiseHp = 0.0f;
        float thudEnv = 0.0f, thudDecay = 0.0f, thudLp = 0.0f, thudLpCoef = 0.1f, thudLevel = 0.0f;
        float buzzLevel = 0.0f;
        // ghost fade (only used in the ghost pool)
        float fade = 1.0f, fadeStep = 0.0f;

        void noteOn(const VoiceStart& s) noexcept;
        void noteOff() noexcept { held = false; }
        void kill() noexcept;
        void reset() noexcept;
        bool isActive() const noexcept { return active; }
        // Adds n mono samples into out.
        void render(float* out, int n) noexcept;

    private:
        void setupNote(bool restrike) noexcept;
        float pickupCurve(float tipDisplacement) const noexcept; // flux / capacitance before d/dt
        void updateControl() noexcept;
        float whiteNoise() noexcept {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return static_cast<float>(static_cast<int32_t>(rng)) * 4.656612873e-10f;
        }
    };

    VoiceAllocator<Voice, kMaxVoices>& allocator() noexcept { return alloc_; }
    // Ghost pool (stolen voices fading out). Called from Voice::kill on the audio thread.
    void adoptGhost(const Voice& v) noexcept;

private:
    void updateShared(const ProcessContext& ctx) noexcept;
    void renderSegment(int start, int end) noexcept;
    void postProcess(AudioBlock& out) noexcept;
    static float pedalLiftFromCc(int cc) noexcept;

    Shared shared_;
    VoiceAllocator<Voice, kMaxVoices> alloc_;
    std::array<Voice, kGhosts> ghosts_{};
    std::atomic<int> activeVoices_{0};

    float* mono_ = nullptr; // == out.left during process
    int pedalCc_ = 0;
    bool pedalInit_ = false;

    // post chain
    dsp::Svf toneLp_, presBp_, bassLp_, hp_;
    float toneHz_ = -1.0f, presHz_ = -1.0f, hpHz_ = -1.0f;
    float presGain_ = 0.0f, bassGain_ = 0.0f, preDrive_ = 1.0f, preBias_ = 0.0f, preNorm_ = 1.0f;
    bool useHp_ = false;
    float dcX1_ = 0.0f, dcY1_ = 0.0f, dcR_ = 0.998f;
    float lfoC_ = 1.0f, lfoS_ = 0.0f;
    dsp::OnePoleSmoother volume_, stereoDepth_;
};

} // namespace ks
