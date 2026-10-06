#include "transport/Pattern.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

namespace ks {

using nlohmann::json;

namespace {

void warn(std::vector<std::string>* w, const std::string& m) {
    if (w) w->push_back(m);
}

int validDen(int d) { return d == 2 || d == 4 || d == 8 || d == 16 ? d : 4; }

// One char -> velocity, -1 = ignored char.
int charVelocity(char c) {
    switch (c) {
    case '.': case '-': case '_': return 0;
    case 'x': return 100;
    case 'X': return 127;
    case 'o': case 'O': return 50;
    case ' ': case '|': case '\t': return -1;
    default: break;
    }
    if (c >= '1' && c <= '9') return static_cast<int>(std::lround((c - '0') * 127.0 / 9.0));
    return -2; // unknown -> treat as off, warn
}

std::vector<uint8_t> parseRow(const json& j, std::vector<std::string>* w, const std::string& where, bool binary) {
    std::vector<uint8_t> out;
    if (j.is_string()) {
        bool bad = false;
        for (char c : j.get<std::string>()) {
            int v = charVelocity(c);
            if (v == -1) continue;
            if (v == -2) {
                bad = true;
                v = 0;
            }
            out.push_back(static_cast<uint8_t>(binary ? (v > 0 ? 1 : 0) : v));
        }
        if (bad) warn(w, where + ": unknown step characters treated as off");
    } else if (j.is_array()) {
        for (const auto& v : j) {
            double d = 0.0;
            if (v.is_number()) d = v.get<double>();
            else if (v.is_boolean()) d = v.get<bool>() ? (binary ? 1 : 100) : 0;
            if (!std::isfinite(d)) d = 0.0;
            const int i = std::clamp(static_cast<int>(std::lround(d)), 0, 127);
            out.push_back(static_cast<uint8_t>(binary ? (i > 0 ? 1 : 0) : i));
        }
    } else if (!j.is_null()) {
        warn(w, where + ": expected a string or an array");
    }
    return out;
}

template <typename T>
bool num(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    const double d = it->get<double>();
    if (!std::isfinite(d)) return false;
    out = static_cast<T>(d);
    return true;
}

} // namespace

int kitIndexFromName(const std::string& sIn) {
    std::string s;
    for (char c : sIn) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (s == "808" || s == "tr-808" || s == "tr808") return 0;
    if (s == "909" || s == "tr-909" || s == "tr909") return 1;
    if (s == "linn" || s == "linndrum" || s == "lm-1") return 2;
    if (s == "industrial" || s == "rammstein") return 3;
    return -1;
}

std::vector<std::string> Pattern::normalize() {
    std::vector<std::string> w;
    numerator = std::clamp(numerator, 1, 16);
    denominator = validDen(denominator);
    stepsPerBeat = std::clamp(stepsPerBeat, 1, 8);
    bars = std::clamp(bars, 1, kPatternMaxBars);
    while (bars > 1 && numSteps() > kPatternMaxSteps) --bars;
    if (numSteps() > kPatternMaxSteps) {
        w.push_back("pattern too long; steps_per_beat reduced");
        while (stepsPerBeat > 1 && numSteps() > kPatternMaxSteps) --stepsPerBeat;
    }
    if (!std::isfinite(swing)) swing = 0.0f;
    swing = std::clamp(swing, 0.0f, 1.0f);
    if (!std::isfinite(accentAmount)) accentAmount = 0.5f;
    accentAmount = std::clamp(accentAmount, 0.0f, 1.0f);
    if (!std::isfinite(tempo) || tempo < 0) tempo = 0.0;
    if (tempo > 0) tempo = std::clamp(tempo, 20.0, 400.0);
    if (kit < -1 || kit > 3) kit = -1;
    if (tracks.size() > static_cast<size_t>(kPatternMaxTracks)) {
        w.push_back("too many tracks; extra tracks dropped");
        tracks.resize(kPatternMaxTracks);
    }
    const size_t n = static_cast<size_t>(numSteps());
    for (auto& t : tracks) {
        t.note = std::clamp(t.note, 0, 127);
        if (t.name.size() > 32) t.name.resize(32);
        for (auto& v : t.steps) v = std::min<uint8_t>(v, 127);
        t.steps.resize(n, 0);
    }
    for (auto& a : accent) a = a ? 1 : 0;
    accent.resize(n, 0);
    if (name.size() > 64) name.resize(64);
    return w;
}

void Pattern::setTimeSignature(int num, int den) {
    numerator = std::clamp(num, 1, 16);
    denominator = validDen(den);
    normalize();
}

Pattern patternFromJson(const json& j, std::vector<std::string>* w) {
    if (!j.is_object()) throw std::invalid_argument("pattern must be an object");
    Pattern p;
    p.name = "Untitled";
    if (auto it = j.find("name"); it != j.end() && it->is_string()) p.name = it->get<std::string>();
    if (auto it = j.find("description"); it != j.end() && it->is_string()) p.description = it->get<std::string>();
    num(j, "tempo", p.tempo);
    if (auto it = j.find("time_sig"); it != j.end()) {
        if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
            p.numerator = static_cast<int>((*it)[0].get<double>());
            p.denominator = static_cast<int>((*it)[1].get<double>());
        } else {
            warn(w, "time_sig: expected [num, den]");
        }
    }
    num(j, "steps_per_beat", p.stepsPerBeat);
    num(j, "bars", p.bars);
    num(j, "swing", p.swing);
    num(j, "accent_amount", p.accentAmount);
    if (auto it = j.find("kit"); it != j.end()) {
        if (it->is_string()) {
            p.kit = kitIndexFromName(it->get<std::string>());
            if (p.kit < 0) warn(w, "kit: unknown kit name");
        } else if (it->is_number()) {
            p.kit = static_cast<int>(it->get<double>());
        }
    }
    if (auto it = j.find("tracks"); it != j.end() && it->is_array()) {
        int i = 0;
        for (const auto& tj : *it) {
            const std::string where = "tracks[" + std::to_string(i++) + "]";
            if (!tj.is_object()) {
                warn(w, where + ": expected an object");
                continue;
            }
            PatternTrack t;
            if (auto n = tj.find("name"); n != tj.end() && n->is_string()) t.name = n->get<std::string>();
            num(tj, "note", t.note);
            if (auto m = tj.find("mute"); m != tj.end() && m->is_boolean()) t.mute = m->get<bool>();
            if (auto s = tj.find("steps"); s != tj.end()) t.steps = parseRow(*s, w, where + ".steps", false);
            if (t.name.empty()) t.name = "Note " + std::to_string(t.note);
            p.tracks.push_back(std::move(t));
        }
    }
    if (auto it = j.find("accent"); it != j.end()) p.accent = parseRow(*it, w, "accent", true);
    if (w) {
        static const std::set<std::string> known = {"format", "name", "description", "tempo", "time_sig",
                                                    "steps_per_beat", "bars", "swing", "accent_amount", "kit",
                                                    "tracks", "accent"};
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!known.count(it.key())) w->push_back("pattern: unknown key '" + it.key() + "' ignored");
    }
    // Infer bars from the longest row when "bars" is missing.
    if (!j.contains("bars")) {
        size_t longest = 0;
        for (const auto& t : p.tracks) longest = std::max(longest, t.steps.size());
        const int spb = std::max(1, std::clamp(p.numerator, 1, 16) * std::clamp(p.stepsPerBeat, 1, 8));
        p.bars = std::max(1, static_cast<int>((longest + static_cast<size_t>(spb) - 1) / static_cast<size_t>(spb)));
    }
    auto nw = p.normalize();
    if (w) w->insert(w->end(), nw.begin(), nw.end());
    return p;
}

json patternToJson(const Pattern& p) {
    json tracks = json::array();
    for (const auto& t : p.tracks) {
        json steps = json::array();
        for (uint8_t v : t.steps) steps.push_back(static_cast<int>(v));
        tracks.push_back({{"name", t.name}, {"note", t.note}, {"mute", t.mute}, {"steps", steps}});
    }
    json accent = json::array();
    for (uint8_t a : p.accent) accent.push_back(static_cast<int>(a));
    json j = {{"format", 1},
              {"name", p.name},
              {"description", p.description},
              {"time_sig", {p.numerator, p.denominator}},
              {"steps_per_beat", p.stepsPerBeat},
              {"bars", p.bars},
              {"swing", p.swing},
              {"accent_amount", p.accentAmount},
              {"tracks", tracks},
              {"accent", accent}};
    if (p.tempo > 0) j["tempo"] = p.tempo;
    if (p.kit >= 0) j["kit"] = p.kit;
    return j;
}

RtPattern toRt(const Pattern& pIn) {
    Pattern p = pIn;
    p.normalize();
    RtPattern r;
    r.numerator = p.numerator;
    r.denominator = p.denominator;
    r.stepsPerBeat = p.stepsPerBeat;
    r.numSteps = p.numSteps();
    r.numTracks = static_cast<int>(p.tracks.size());
    const int boost = static_cast<int>(std::lround(p.accentAmount * 64.0f));
    for (int t = 0; t < r.numTracks; ++t) {
        const PatternTrack& src = p.tracks[static_cast<size_t>(t)];
        RtPattern::Track& dst = r.tracks[static_cast<size_t>(t)];
        dst.note = static_cast<uint8_t>(src.note);
        dst.mute = src.mute;
        for (int s = 0; s < r.numSteps; ++s) {
            int v = src.steps[static_cast<size_t>(s)];
            if (v > 0 && p.accent[static_cast<size_t>(s)]) v = std::min(127, v + boost);
            dst.vel[static_cast<size_t>(s)] = static_cast<uint8_t>(v);
        }
    }
    return r;
}

} // namespace ks
