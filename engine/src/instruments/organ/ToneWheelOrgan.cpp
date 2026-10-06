#include "instruments/organ/ToneWheelOrgan.h"

#include "dsp/Math.h"

#include <algorithm>
#include <cmath>

// Model notes
// -----------
// Tonewheels: f = 20 rps * (driving/driven gear teeth) * wheel teeth. Twelve gear pairs per semitone class
// (the real B-3 ratios, so tuning is the Hammond's near-ET, not exact ET; A = 440.0 exactly). Wheels 1-84 have
// 2,4,...,128 teeth per octave; the top seven (85-91) have 192 teeth on the F..B gears (a fifth above).
// Low wheels are not pure sines (tooth profile / magnet saturation): wheels 1-36 get small 2nd/3rd harmonics.
// Upper wheels pass through the tone-filter capacitors -> a gentle level taper; +-0.4 dB fixed per-wheel
// variation (wheel/pickup tolerance).
// Manual: key k (0..60) 8' = wheel 13+k. Drawbar offsets (semitones): 16' -12, 5 1/3' +7, 8' 0, 4' +12,
// 2 2/3' +19, 2' +24, 1 3/5' +28, 1 1/3' +31, 1' +36. Foldback like the B-3 manual: the bottom octave of the 16'
// repeats the next octave (wheels 1-12 are only for pedals); footages that would need a wheel above 91 drop an
// octave (top octave of 1', 1 1/3', 1 3/5', 2', 2 2/3').
// Contacts: each key's 9 contacts make/break at slightly different times (spread ~0..2.5 ms scaled by `click`)
// with a short bounce; a sharp make on a running wheel is the classic key click. `click` also adds a filtered
// noise burst (contact bounce noise) and shortens the contact ramp.
// Percussion: one shared envelope, single-trigger (re-armed only when all keys are up; legato playing does not
// retrigger), taken from the 4' (2nd) or 2 2/3' (3rd) wheel of every held key; fast/slow decay, normal/soft
// volume (normal drops the drawbar level ~3 dB, like the original); the 1' drawbar is cancelled while on.
// Leakage: every keyed wheel bleeds into its generator-compartment partner (+-48 wheels) and its neighbours.

namespace ks {

namespace {

constexpr int kRatioNum[12] = {85, 71, 67, 105, 103, 84, 74, 98, 96, 88, 67, 108};
constexpr int kRatioDen[12] = {104, 82, 73, 108, 100, 77, 64, 80, 74, 64, 46, 70};
constexpr int kBarOffset[ToneWheelOrgan::kBars] = {-12, 7, 0, 12, 19, 24, 28, 31, 36};

int foldNote(int note) noexcept {
    while (note < ToneWheelOrgan::kLowNote) note += 12;
    while (note > ToneWheelOrgan::kLowNote + ToneWheelOrgan::kKeys - 1) note -= 12;
    return note;
}

} // namespace

const ModuleInfo& ToneWheelOrgan::moduleInfo() {
    static const ModuleInfo info = [] {
        ModuleInfo i;
        i.typeId = "organ";
        i.displayName = "Tonewheel Organ";
        i.kind = ModuleKind::Instrument;
        i.category = "Organ";
        i.params = {
            intParam("db_16", "16'", 0, 8, 8, {}, "Drawbars"),
            intParam("db_5_1_3", "5 1/3'", 0, 8, 8, {}, "Drawbars"),
            intParam("db_8", "8'", 0, 8, 8, {}, "Drawbars"),
            intParam("db_4", "4'", 0, 8, 0, {}, "Drawbars"),
            intParam("db_2_2_3", "2 2/3'", 0, 8, 0, {}, "Drawbars"),
            intParam("db_2", "2'", 0, 8, 0, {}, "Drawbars"),
            intParam("db_1_3_5", "1 3/5'", 0, 8, 0, {}, "Drawbars"),
            intParam("db_1_1_3", "1 1/3'", 0, 8, 0, {}, "Drawbars"),
            intParam("db_1", "1'", 0, 8, 0, {}, "Drawbars"),
            boolParam("perc", "Percussion", false, "Percussion"),
            enumParam("perc_harmonic", "Harmonic", {"Second", "Third"}, 0, "Percussion"),
            enumParam("perc_decay", "Decay", {"Fast", "Slow"}, 0, "Percussion"),
            enumParam("perc_volume", "Volume", {"Normal", "Soft"}, 0, "Percussion"),
            enumParam("vibrato", "Vibrato/Chorus", {"Off", "V1", "V2", "V3", "C1", "C2", "C3"}, 0, "Vibrato"),
            linearParam("click", "Key Click", 0.0f, 1.0f, 0.5f, {}, "Character"),
            linearParam("leakage", "Leakage", 0.0f, 1.0f, 0.3f, {}, "Character"),
            linearParam("drive", "Preamp Drive", 0.0f, 1.0f, 0.0f, {}, "Character"),
            linearParam("volume_db", "Volume", -40.0f, 6.0f, 0.0f, "dB", "Output"),
        };
        nlohmann::json controls = nlohmann::json::object();
        for (int b = 0; b < kBars; ++b) controls[i.params[static_cast<size_t>(b)].id] = "drawbar";
        i.uiHints = {{"groupOrder", {"Drawbars", "Percussion", "Vibrato", "Character", "Output"}},
                     {"front", {"db_16", "db_5_1_3", "db_8", "db_4", "db_2_2_3", "db_2", "db_1_3_5", "db_1_1_3",
                                "db_1", "perc", "vibrato", "drive"}},
                     {"controls", controls}};
        return i;
    }();
    return info;
}

double ToneWheelOrgan::wheelFrequency(int wheel) noexcept {
    wheel = std::clamp(wheel, 1, kWheels);
    int cls, teeth;
    if (wheel <= 84) {
        cls = (wheel - 1) % 12;
        teeth = 2 << ((wheel - 1) / 12);
    } else {
        cls = 5 + (wheel - 85); // F..B gears, 192 teeth
        teeth = 192;
    }
    return 20.0 * static_cast<double>(kRatioNum[cls]) / static_cast<double>(kRatioDen[cls]) * teeth;
}

int ToneWheelOrgan::wheelFor(int midiNote, int bar) noexcept {
    const int k = foldNote(midiNote) - kLowNote;
    bar = std::clamp(bar, 0, kBars - 1);
    int w = 13 + k + kBarOffset[bar];
    if (bar == Db16 && k < 12) w += 12; // bottom-octave 16' foldback
    while (w > kWheels) w -= 12;         // top foldback
    return w;
}

float ToneWheelOrgan::drawbarGain(float position) noexcept {
    if (position < 0.5f) return 0.0f;
    return std::exp2((std::min(position, 8.0f) - 8.0f) * 0.5f);
}

ToneWheelOrgan::ToneWheelOrgan() : Module(moduleInfo()) {
    // Tooth-profile harmonics for low wheels; pure sines above wheel 36.
    tables_[0].build({1.0f, 0.04f, 0.12f, 0.0f, 0.03f});
    tables_[1].build({1.0f, 0.025f, 0.045f});
    tables_[2].build({1.0f, 0.01f, 0.015f});
    tables_[3].build({1.0f});
    dsp::XorNoise rng(0x0B3B3u);
    for (int w = 1; w <= kWheels; ++w) {
        wheelTable_[static_cast<size_t>(w)] = static_cast<uint8_t>(w <= 12 ? 0 : w <= 24 ? 1 : w <= 36 ? 2 : 3);
        const float taperDb = w > 48 ? -0.12f * static_cast<float>(w - 48) : 0.0f;
        const float tolDb = 0.4f * rng.next();
        wheelGain_[static_cast<size_t>(w)] = dsp::dbToGain(taperDb + tolDb);
        phase_[static_cast<size_t>(w)] = rng.nextU32(); // wheels are never in phase
    }
    for (int k = 0; k < kKeys; ++k)
        for (int b = 0; b < kBars; ++b)
            wheelMap_[static_cast<size_t>(k)][static_cast<size_t>(b)] =
                static_cast<uint8_t>(wheelFor(kLowNote + k, b));
}

void ToneWheelOrgan::prepare(double sampleRate, int /*maxBlock*/) {
    sampleRate_ = sampleRate;
    for (int w = 1; w <= kWheels; ++w)
        inc_[static_cast<size_t>(w)] = dsp::PhaseTable::increment(wheelFrequency(w), sampleRate);
    clickDecay_ = std::exp(-1.0f / (0.0025f * static_cast<float>(sampleRate)));
    clickFilter_.setSampleRate(sampleRate);
    clickFilter_.setCutoff(2800.0f, 0.25f);
    scanner_.prepare(sampleRate);
    preamp_.prepare(sampleRate);
    outGain_.prepare(sampleRate, 0.02f);
    ProcessContext ctx;
    ctx.sampleRate = sampleRate;
    updateBlockParams(ctx);
    outGain_.snap(outGain_.target());
    reset();
}

void ToneWheelOrgan::reset() {
    for (auto& k : keys_) k = Key{};
    cur_.fill(0.0f);
    target_.fill(0.0f);
    keysDown_ = 0;
    percEnv_ = 0.0f;
    clickEnv_ = 0.0f;
    clickFilter_.reset();
    scanner_.reset();
    preamp_.reset();
    activeKeys_.store(0, std::memory_order_relaxed);
}

void ToneWheelOrgan::updateBlockParams(const ProcessContext& ctx) noexcept {
    const ParamSet& p = params();
    percOn_ = p.get(Perc) >= 0.5f;
    barTotal_ = 0.0f;
    for (int b = 0; b < kBars; ++b) {
        barGain_[b] = drawbarGain(p.get(b));
        if (b == Db1 && percOn_) barGain_[b] = 0.0f; // percussion steals the 1' contact busbar
        barTotal_ += barGain_[b];
    }
    percBar_ = p.get(PercHarmonic) >= 0.5f ? Db223 : Db4;
    percTau_ = p.get(PercDecay) >= 0.5f ? 0.58f : 0.145f;
    const bool soft = p.get(PercVolume) >= 0.5f;
    percGain_ = percOn_ ? (soft ? 0.45f : 1.0f) : 0.0f;
    const float barDrop = percOn_ && !soft ? 0.71f : 1.0f;
    // Busbar loading: many drawbars out do not sum linearly.
    const float loading = 1.0f / std::sqrt(1.0f + 0.15f * std::max(0.0f, barTotal_ - 1.0f));
    for (float& g : barGain_) g *= barDrop * loading;
    percGain_ *= loading;

    click_ = p.get(Click);
    leak_ = p.get(Leakage);
    drive_ = p.get(Drive);
    const float sr = static_cast<float>(sampleRate_);
    rampSamples_ = std::max(1, static_cast<int>((0.006f - 0.0057f * click_) * sr));
    spreadSamples_ = static_cast<int>(0.0025f * click_ * sr);
    bounceSamples_ = click_ > 0.25f ? static_cast<int>(0.0008f * sr) : 0;
    scanner_.setMode(static_cast<int>(p.get(Vibrato)));
    preamp_.setDrive(drive_);

    const float expr = ctx.channel ? std::clamp(ctx.channel->expression, 0.0f, 1.0f) : 1.0f;
    // Swell pedal: ~ -30 dB at heel-down, roughly exponential.
    const float swell = expr <= 0.0f ? 0.0f : dsp::dbToGain(-30.0f * (1.0f - expr));
    outGain_.setTarget(0.14f * dsp::dbToGain(p.get(VolumeDb)) * swell);
}

void ToneWheelOrgan::keyDown(int k) noexcept {
    Key& key = keys_[static_cast<size_t>(k)];
    if (key.down) return;
    if (keysDown_ == 0 && percOn_) {
        percEnv_ = 1.0f; // single trigger: only when no other key is held
        ++percTriggers_;
    }
    ++keysDown_;
    key.down = true;
    key.active = true;
    key.t = 0;
    key.bounce = noise_.nextU32();
    for (int b = 0; b < kBars; ++b)
        key.delay[b] = static_cast<int>(static_cast<float>(spreadSamples_) * (0.5f + 0.5f * noise_.next()));
    // Contact-bounce noise burst, scaled by how much is drawn out.
    clickEnv_ += click_ * click_ * 0.12f * std::min(1.0f, barTotal_ * 0.5f);
}

void ToneWheelOrgan::keyUp(int k) noexcept {
    Key& key = keys_[static_cast<size_t>(k)];
    if (!key.down) return;
    key.down = false;
    keysDown_ = std::max(0, keysDown_ - 1);
    key.t = 0;
    for (int b = 0; b < kBars; ++b)
        key.delay[b] = static_cast<int>(static_cast<float>(spreadSamples_) * 0.6f * (0.5f + 0.5f * noise_.next()));
    clickEnv_ += click_ * click_ * 0.05f * std::min(1.0f, barTotal_ * 0.5f);
}

void ToneWheelOrgan::handleEvent(const MidiEvent& e) noexcept {
    switch (e.type) {
    case MidiEventType::NoteOn: keyDown(foldNote(e.data1) - kLowNote); break;
    case MidiEventType::NoteOff: keyUp(foldNote(e.data1) - kLowNote); break;
    case MidiEventType::AllNotesOff:
        for (int k = 0; k < kKeys; ++k) keyUp(k);
        break;
    case MidiEventType::AllSoundOff: reset(); break;
    default: break;
    }
}

void ToneWheelOrgan::renderChunk(float* dst, int len) noexcept {
    // 1. Advance contacts, build per-wheel target gains.
    keyed_.fill(0.0f);
    const float step = static_cast<float>(len) / static_cast<float>(rampSamples_);
    int active = 0;
    for (int k = 0; k < kKeys; ++k) {
        Key& key = keys_[static_cast<size_t>(k)];
        if (!key.active) continue;
        bool any = key.down;
        for (int b = 0; b < kBars; ++b) {
            float& v = key.v[b];
            const int since = key.t - key.delay[b];
            if (key.down) {
                if (since >= 0) {
                    if (since < bounceSamples_) {
                        // Bounce: contact chatters for ~1 ms after first make.
                        const uint32_t bit = (key.bounce >> ((b * 3 + since / kChunk) & 31)) & 1u;
                        v = bit ? 1.0f : 0.25f;
                    } else {
                        v = std::min(1.0f, v + step);
                    }
                }
            } else if (since >= 0) {
                v = std::max(0.0f, v - step);
            }
            if (v > 0.0f) {
                any = true;
                const auto w = wheelMap_[static_cast<size_t>(k)][static_cast<size_t>(b)];
                keyed_[w] += v * barGain_[b];
                if (b == percBar_) keyed_[w] += v * percGain_ * percEnv_;
            }
        }
        key.t += len;
        key.active = any;
        active += any ? 1 : 0;
    }
    activeKeys_.store(active, std::memory_order_relaxed);
    if (percEnv_ > 0.0f) {
        percEnv_ *= std::exp(-static_cast<float>(len) / (percTau_ * static_cast<float>(sampleRate_)));
        if (percEnv_ < 1e-5f) percEnv_ = 0.0f;
    }

    // 2. Leakage: compartment partner (+-48 wheels) and adjacent wheels pick up a little of every keyed wheel.
    const float lp = leak_ * 0.02f, ln = leak_ * 0.006f;
    for (int w = 1; w <= kWheels; ++w) {
        float t = keyed_[static_cast<size_t>(w)];
        if (lp > 0.0f && active > 0) {
            if (w > 48) t += lp * keyed_[static_cast<size_t>(w - 48)];
            if (w + 48 <= kWheels) t += lp * keyed_[static_cast<size_t>(w + 48)];
            if (w > 1) t += ln * keyed_[static_cast<size_t>(w - 1)];
            if (w < kWheels) t += ln * keyed_[static_cast<size_t>(w + 1)];
        }
        target_[static_cast<size_t>(w)] = t * wheelGain_[static_cast<size_t>(w)];
    }

    // 3. Run the wheels.
    std::fill(dst, dst + len, 0.0f);
    const float invLen = 1.0f / static_cast<float>(len);
    for (int w = 1; w <= kWheels; ++w) {
        const size_t wi = static_cast<size_t>(w);
        uint32_t ph = phase_[wi];
        const uint32_t inc = inc_[wi];
        float g = cur_[wi];
        const float tg = target_[wi];
        if (g == 0.0f && tg == 0.0f) {
            phase_[wi] = ph + inc * static_cast<uint32_t>(len);
            continue;
        }
        const float dg = (tg - g) * invLen;
        const dsp::PhaseTable& tab = tables_[wheelTable_[wi]];
        for (int i = 0; i < len; ++i) {
            g += dg;
            dst[i] += g * tab.lookup(ph);
            ph += inc;
        }
        phase_[wi] = ph;
        cur_[wi] = tg;
    }
}

void ToneWheelOrgan::process(AudioBlock& out, MidiEventSpan events, const ProcessContext& ctx) {
    updateBlockParams(ctx);
    float* buf = out.left;
    int pos = 0;
    size_t ev = 0;
    while (pos < out.numSamples) {
        while (ev < events.size() && static_cast<int>(events[ev].sampleOffset) <= pos) handleEvent(events[ev++]);
        int end = std::min(out.numSamples, pos + kChunk);
        if (ev < events.size()) end = std::min(end, std::max(pos + 1, static_cast<int>(events[ev].sampleOffset)));
        renderChunk(buf + pos, end - pos);
        pos = end;
    }
    while (ev < events.size()) handleEvent(events[ev++]);

    const bool drive = drive_ > 0.001f;
    for (int i = 0; i < out.numSamples; ++i) {
        float x = buf[i];
        if (clickEnv_ > 1e-6f) {
            x += clickFilter_.tick(noise_.next() * clickEnv_).bp;
            clickEnv_ *= clickDecay_;
        } else {
            clickEnv_ = 0.0f;
        }
        x = scanner_.process(x);
        const float g = outGain_.next();
        // Preamp: drive works on the pre-volume signal (organ preamp sits before the swell pedal).
        if (drive) x = preamp_.process(x * 0.25f) * 4.0f;
        buf[i] = x * g;
    }
    std::copy(out.left, out.left + out.numSamples, out.right);
}

} // namespace ks
