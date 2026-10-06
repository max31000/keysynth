#pragma once
// ADSR envelope. Linear attack; exponential decay/release (one-pole towards a target slightly past the goal so
// stages finish in finite time). Retrigger starts the attack from the current level (no click).
// kill(): fast ~3 ms release used for voice stealing.

#include "dsp/Math.h"

namespace ks::dsp {

class Adsr {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    struct Params {
        float attack = 0.005f;  // s
        float decay = 0.2f;     // s
        float sustain = 0.7f;   // 0..1
        float release = 0.3f;   // s
    };

    void setSampleRate(double sr) noexcept { sampleRate_ = sr; }

    void setParams(const Params& p) noexcept {
        params_ = p;
        attackInc_ = 1.0f / std::fmax(1.0f, p.attack * static_cast<float>(sampleRate_));
        decayCoef_ = expCoef(p.decay);
        releaseCoef_ = expCoef(p.release);
    }

    void noteOn() noexcept { stage_ = Stage::Attack; }
    void noteOff() noexcept {
        if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
            releaseCoefActive_ = releaseCoef_;
        }
    }
    void kill() noexcept {
        if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
            releaseCoefActive_ = expCoef(0.003f);
        }
    }
    void reset() noexcept {
        stage_ = Stage::Idle;
        level_ = 0.0f;
    }

    bool isActive() const noexcept { return stage_ != Stage::Idle; }
    Stage stage() const noexcept { return stage_; }
    float level() const noexcept { return level_; }

    float next() noexcept {
        switch (stage_) {
        case Stage::Idle: return 0.0f;
        case Stage::Attack:
            level_ += attackInc_;
            if (level_ >= 1.0f) {
                level_ = 1.0f;
                stage_ = Stage::Decay;
            }
            break;
        case Stage::Decay: {
            const float target = params_.sustain - kOvershoot;
            level_ += (target - level_) * decayCoef_;
            if (level_ <= params_.sustain) {
                level_ = params_.sustain;
                stage_ = Stage::Sustain;
            }
            break;
        }
        case Stage::Sustain:
            // Follow sustain-level changes smoothly.
            level_ += (params_.sustain - level_) * 0.001f;
            break;
        case Stage::Release:
            level_ += (-kOvershoot - level_) * releaseCoefActive_;
            if (level_ <= 0.0f) {
                level_ = 0.0f;
                stage_ = Stage::Idle;
            }
            break;
        }
        return level_;
    }

private:
    static constexpr float kOvershoot = 0.001f;
    float expCoef(float seconds) const noexcept {
        // Reach (target) from 1 within `seconds`: ln((1+o)/o) time constants.
        const float tc = std::fmax(1e-4f, seconds) / std::log((1.0f + kOvershoot) / kOvershoot);
        return onePoleCoef(tc, sampleRate_);
    }

    double sampleRate_ = 48000.0;
    Params params_{};
    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    float attackInc_ = 0.001f;
    float decayCoef_ = 0.001f;
    float releaseCoef_ = 0.001f;
    float releaseCoefActive_ = 0.001f;
};

} // namespace ks::dsp
