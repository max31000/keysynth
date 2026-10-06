#pragma once
// 4-pole zero-delay-feedback ladder (Zavalishin, "The Art of VA Filter Design", ch. 5) with a cheap
// non-linearity: tanh in the loop is replaced by its secant slope evaluated at the linear solution, so the
// loop is solved in closed form every sample and stays stable for any feedback (resonance up to and past
// self-oscillation; the tanh bounds the oscillation amplitude).
//
// Models:
//   Transistor - Moog-style: saturation at the differential-pair input (tanh(x - k*y4)); bass drops as
//                resonance rises, as on the original; partial gain compensation.
//   Ota        - IR3109 (Juno/Jupiter)-style: clean input path, saturation in the feedback path; resonance
//                peak is softer and the bass loss is compensated more (Roland's resonance gain makeup).
// Responses are mixed from stage outputs (Oberheim Xpander style): LP24, BP (2+2 pole), HP24, notch.
// Coefficient: G = g/(1+g), g = tan(pi fc/fs) (see coefG()); callers may interpolate G per sample.

#include "dsp/FastMath.h"
#include "dsp/Math.h"

namespace ks::dsp {

class LadderFilter {
public:
    enum class Model { Transistor = 0, Ota = 1 };
    enum class Response { LowPass = 0, BandPass = 1, HighPass = 2, Notch = 3 };

    void reset() noexcept { s1_ = s2_ = s3_ = s4_ = 0.0f; }

    // Feedback k for resonance r in 0..1 (self-oscillation starts just below r = 1).
    static float feedbackFor(float r, Model m) noexcept {
        r = std::fmin(std::fmax(r, 0.0f), 1.0f);
        return (m == Model::Transistor ? 4.15f : 4.3f) * r;
    }

    // One-pole TPT gain for cutoff fc at sample rate fs (fc clamped below 0.49 fs).
    static float coefG(float fc, float fs) noexcept {
        const float g = std::tan(kPi * std::fmin(std::fmax(fc, 1.0f), 0.49f * fs) / fs);
        return g / (1.0f + g);
    }

    // G = g/(1+g), g = tan(pi fc / fs) (prewarped); k = feedback (0..~4.3).
    float process(float x, float G, float k, Model m, Response resp) noexcept {
        const float oneMinusG = 1.0f - G;
        const float b1 = oneMinusG * s1_, b2 = oneMinusG * s2_, b3 = oneMinusG * s3_, b4 = oneMinusG * s4_;
        const float G2 = G * G;
        const float G4 = G2 * G2;
        const float S = G2 * G * b1 + G2 * b2 + G * b3 + b4; // y4 = G4 * u + S
        float u;
        if (m == Model::Transistor) {
            // u = tanh(x - k*y4) linearised: u = t (x - k S) / (1 + t k G4)
            const float uLin = (x - k * S) / (1.0f + k * G4);
            const float t = tanhSecant(uLin);
            u = t * (x - k * S) / (1.0f + t * k * G4);
        } else {
            // u = tanh(x) - k * tanh(y4) linearised around the linear y4 estimate.
            const float xs = fastTanh(x);
            const float uLin = (xs - k * S) / (1.0f + k * G4);
            const float t = tanhSecant(G4 * uLin + S);
            u = (xs - k * t * S) / (1.0f + k * t * G4);
        }
        // Four TPT one-poles.
        float v = (u - s1_) * G;
        const float y1 = v + s1_;
        s1_ = y1 + v;
        v = (y1 - s2_) * G;
        const float y2 = v + s2_;
        s2_ = y2 + v;
        v = (y2 - s3_) * G;
        const float y3 = v + s3_;
        s3_ = y3 + v;
        v = (y3 - s4_) * G;
        const float y4 = v + s4_;
        s4_ = y4 + v;

        // Makeup for the resonance bass loss (DC gain of the linear loop is 1/(1+k)). Only the low-frequency
        // (y4) part loses level: HP/BP passbands are not attenuated by the feedback, so they stay uncompensated.
        const float comp = 1.0f + k * (m == Model::Transistor ? 0.35f : 0.6f);
        const float hp = u - 4.0f * y1 + 6.0f * y2 - 4.0f * y3 + y4;
        switch (resp) {
        case Response::LowPass: return y4 * comp;
        case Response::BandPass: return 4.0f * (y2 - 2.0f * y3 + y4);
        case Response::HighPass: return hp;
        case Response::Notch: return hp + y4 * comp;
        }
        return y4;
    }

private:
    float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f;
};

} // namespace ks::dsp
