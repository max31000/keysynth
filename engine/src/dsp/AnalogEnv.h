#pragma once
// Analog-style ADSR (CEM3310 / IR3R01 behaviour): RC-curve attack aiming past full scale (1.3x, so the
// attack is concave and ends with a finite slope), RC decay/release towards their targets. Retrigger starts
// from the current level. Stage times are the time to complete the stage from full scale.
//
// Coefficients live in a separate `Coefs` struct so one computation per block serves every voice; the
// state is 3 words. Run per sample (amp) or at control rate (pass the control rate to makeCoefs).

#include "dsp/Math.h"

namespace ks::dsp {

class AnalogEnv {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    struct Params {
        float attack = 0.005f; // s
        float decay = 0.3f;    // s
        float sustain = 0.7f;  // 0..1
        float release = 0.3f;  // s
    };

    struct Coefs {
        float attack = 1.0f, decay = 1.0f, release = 1.0f, kill = 1.0f;
        float sustain = 0.7f;
    };

    static constexpr float kAttackTarget = 1.3f;
    static constexpr float kEps = 0.003f; // decay/release aim this far past their target (~ -50 dB)

    static Coefs makeCoefs(const Params& p, double rate) noexcept {
        Coefs c;
        const float attackTc = std::fmax(p.attack, 1e-5f) / std::log(kAttackTarget / (kAttackTarget - 1.0f));
        const float drTc = 1.0f / std::log((1.0f + kEps) / kEps);
        c.attack = onePoleCoef(attackTc, rate);
        c.decay = onePoleCoef(std::fmax(p.decay, 1e-4f) * drTc, rate);
        c.release = onePoleCoef(std::fmax(p.release, 1e-4f) * drTc, rate);
        c.kill = onePoleCoef(0.003f * drTc, rate);
        c.sustain = std::fmin(std::fmax(p.sustain, 0.0f), 1.0f);
        return c;
    }

    void noteOn() noexcept { stage_ = Stage::Attack; }
    void noteOff() noexcept {
        if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
            killing_ = false;
        }
    }
    void kill() noexcept {
        if (stage_ != Stage::Idle) {
            stage_ = Stage::Release;
            killing_ = true;
        }
    }
    void reset() noexcept {
        stage_ = Stage::Idle;
        level_ = 0.0f;
        killing_ = false;
    }

    bool isActive() const noexcept { return stage_ != Stage::Idle; }
    Stage stage() const noexcept { return stage_; }
    float level() const noexcept { return level_; }

    float next(const Coefs& c) noexcept {
        switch (stage_) {
        case Stage::Idle: return 0.0f;
        case Stage::Attack:
            level_ += (kAttackTarget - level_) * c.attack;
            if (level_ >= 1.0f) {
                level_ = 1.0f;
                stage_ = Stage::Decay;
            }
            break;
        case Stage::Decay:
            level_ += (c.sustain - kEps - level_) * c.decay;
            if (level_ <= c.sustain) {
                level_ = c.sustain;
                stage_ = Stage::Sustain;
            }
            break;
        case Stage::Sustain: level_ += (c.sustain - level_) * c.decay; break; // follows sustain changes
        case Stage::Release:
            level_ += (-kEps - level_) * (killing_ ? c.kill : c.release);
            if (level_ <= 0.0f) {
                level_ = 0.0f;
                stage_ = Stage::Idle;
                killing_ = false;
            }
            break;
        }
        return level_;
    }

private:
    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    bool killing_ = false;
};

} // namespace ks::dsp
