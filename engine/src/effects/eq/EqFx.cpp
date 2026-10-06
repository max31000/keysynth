#include "effects/eq/EqFx.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace ks {

const ModuleInfo& EqFx::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "eq";
        i.displayName = "EQ";
        i.kind = ModuleKind::Effect;
        i.category = "EQ";
        i.params = {
            boolParam("hp_on", "HP On", false, "Cut"),
            logParam("hp_hz", "HP Freq", 10.0f, 1000.0f, 40.0f, "Hz", "Cut", 80.0f),
            boolParam("lp_on", "LP On", false, "Cut"),
            logParam("lp_hz", "LP Freq", 1000.0f, 20000.0f, 16000.0f, "Hz", "Cut", 6000.0f),
            enumParam("cut_slope", "Cut Slope", {"12 dB/oct", "24 dB/oct"}, 0, "Cut"),
            logParam("ls_hz", "Low Shelf Freq", 20.0f, 1000.0f, 100.0f, "Hz", "Low Shelf", 150.0f),
            linearParam("ls_db", "Low Shelf Gain", -18.0f, 18.0f, 0.0f, "dB", "Low Shelf"),
            logParam("p1_hz", "Low Mid Freq", 20.0f, 20000.0f, 250.0f, "Hz", "Low Mid", 630.0f),
            linearParam("p1_db", "Low Mid Gain", -18.0f, 18.0f, 0.0f, "dB", "Low Mid"),
            logParam("p1_q", "Low Mid Q", 0.1f, 10.0f, 1.0f, {}, "Low Mid", 1.0f),
            logParam("p2_hz", "Mid Freq", 20.0f, 20000.0f, 1000.0f, "Hz", "Mid", 630.0f),
            linearParam("p2_db", "Mid Gain", -18.0f, 18.0f, 0.0f, "dB", "Mid"),
            logParam("p2_q", "Mid Q", 0.1f, 10.0f, 1.0f, {}, "Mid", 1.0f),
            logParam("p3_hz", "High Mid Freq", 20.0f, 20000.0f, 4000.0f, "Hz", "High Mid", 630.0f),
            linearParam("p3_db", "High Mid Gain", -18.0f, 18.0f, 0.0f, "dB", "High Mid"),
            logParam("p3_q", "High Mid Q", 0.1f, 10.0f, 1.0f, {}, "High Mid", 1.0f),
            logParam("hs_hz", "High Shelf Freq", 1000.0f, 20000.0f, 8000.0f, "Hz", "High Shelf", 5000.0f),
            linearParam("hs_db", "High Shelf Gain", -18.0f, 18.0f, 0.0f, "dB", "High Shelf"),
            linearParam("level_db", "Level", -24.0f, 12.0f, 0.0f, "dB"),
        };
        i.uiHints = {{"groupOrder", {"Cut", "Low Shelf", "Low Mid", "Mid", "High Mid", "High Shelf"}}};
        return i;
    }();
    return info;
}

EqFx::EqFx() : Module(moduleInfo()) {}

namespace {
struct BandParams {
    int on, hz, db, q; // -1 = none
};
constexpr BandParams kBandParams[] = {
    {EqFx::HpOn, EqFx::HpHz, -1, -1},          {-1, EqFx::LsHz, EqFx::LsDb, -1},
    {-1, EqFx::P1Hz, EqFx::P1Db, EqFx::P1Q},   {-1, EqFx::P2Hz, EqFx::P2Db, EqFx::P2Q},
    {-1, EqFx::P3Hz, EqFx::P3Db, EqFx::P3Q},   {-1, EqFx::HsHz, EqFx::HsDb, -1},
    {EqFx::LpOn, EqFx::LpHz, -1, -1},
};
} // namespace

void EqFx::prepare(double sampleRate, int) {
    sr_ = sampleRate;
    const double subRate = sampleRate / kSub;
    for (auto& b : bands_) {
        b.hz.prepare(subRate, 0.03f);
        b.db.prepare(subRate, 0.03f);
        b.q.prepare(subRate, 0.03f);
    }
    level_.prepare(sampleRate, 0.02f);
    reset();
}

void EqFx::reset() {
    for (int id = 0; id < NumBands; ++id) {
        Band& b = bands_[static_cast<size_t>(id)];
        const BandParams& p = kBandParams[id];
        for (auto& st : b.f)
            for (auto& f : st) f.reset();
        b.hz.snap(std::log2(params().get(p.hz)));
        b.db.snap(p.db >= 0 ? params().get(p.db) : 0.0f);
        b.q.snap(p.q >= 0 ? params().get(p.q) : 0.7071f);
        b.active = false;
        b.stages = 0;
    }
    level_.snap(dsp::dbToGain(params().get(LevelDb)));
}

int EqFx::tailSamples() const { return static_cast<int>(0.05 * sr_); }

void EqFx::configure(Band& b, BandId id, float sr) noexcept {
    using T = dsp::TptSvf::Type;
    const float hz = std::exp2(b.hz.next());
    const float db = b.db.next();
    const float q = b.q.next();
    const bool cut = id == Hp || id == Lp;
    bool active;
    int stages = 1;
    if (cut) {
        active = params().get(kBandParams[id].on) > 0.5f;
        stages = static_cast<int>(params().get(CutSlope)) == 1 ? 2 : 1;
    } else {
        active = !(db == 0.0f && b.db.target() == 0.0f);
    }
    if (active && (!b.active || stages != b.stages))
        for (auto& st : b.f)
            for (auto& f : st) f.reset(); // entering from bypass: start from rest
    b.active = active;
    b.stages = stages;
    if (!active) return;
    for (int c = 0; c < 2; ++c) {
        switch (id) {
        case Hp:
        case Lp: {
            const T t = id == Hp ? T::HighPass : T::LowPass;
            if (stages == 1) {
                b.f[0][c].set(t, hz, 0.7071f, 0.0f, sr);
            } else {
                b.f[0][c].set(t, hz, 0.5412f, 0.0f, sr);
                b.f[1][c].set(t, hz, 1.3066f, 0.0f, sr);
            }
            break;
        }
        case Ls: b.f[0][c].set(T::LowShelf, hz, 0.7071f, db, sr); break;
        case Hs: b.f[0][c].set(T::HighShelf, hz, 0.7071f, db, sr); break;
        default: b.f[0][c].set(T::Bell, hz, q, db, sr); break;
        }
    }
}

void EqFx::process(AudioBlock& io, MidiEventSpan, const ProcessContext&) {
    const float sr = static_cast<float>(sr_);
    for (int id = 0; id < NumBands; ++id) {
        Band& b = bands_[static_cast<size_t>(id)];
        const BandParams& p = kBandParams[id];
        b.hz.setTarget(std::log2(params().get(p.hz)));
        if (p.db >= 0) b.db.setTarget(params().get(p.db));
        if (p.q >= 0) b.q.setTarget(params().get(p.q));
    }
    level_.setTarget(dsp::dbToGain(params().get(LevelDb)));

    for (int s = 0; s < io.numSamples; s += kSub) {
        const int e = std::min(io.numSamples, s + kSub);
        for (int id = 0; id < NumBands; ++id) configure(bands_[static_cast<size_t>(id)], static_cast<BandId>(id), sr);
        for (int c = 0; c < 2; ++c) {
            float* x = io.channel(c);
            for (auto& b : bands_) {
                if (!b.active) continue;
                for (int st = 0; st < b.stages; ++st) {
                    dsp::TptSvf& f = b.f[st][c];
                    for (int i = s; i < e; ++i) x[i] = f.process(x[i]);
                }
            }
        }
        for (int i = s; i < e; ++i) {
            const float g = level_.next();
            io.left[i] *= g;
            io.right[i] *= g;
        }
    }
}

} // namespace ks
