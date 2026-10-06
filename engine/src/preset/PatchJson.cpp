#include "preset/PatchJson.h"

#include "preset/Migrations.h"

#include <cmath>
#include <set>

namespace ks {

using nlohmann::json;

namespace {

void warn(std::vector<std::string>* w, const std::string& msg) {
    if (w) w->push_back(msg);
}

template <typename T>
bool readNum(const json& j, const char* key, T& out, std::vector<std::string>* w, const std::string& where) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return false;
    if (it->is_number()) {
        const double d = it->get<double>();
        if (!std::isfinite(d)) {
            warn(w, where + "." + key + ": not finite, ignored");
            return false;
        }
        out = static_cast<T>(d);
        return true;
    }
    if (it->is_boolean()) {
        out = static_cast<T>(it->get<bool>() ? 1 : 0);
        return true;
    }
    warn(w, where + "." + key + ": expected a number, ignored");
    return false;
}

bool readBool(const json& j, const char* key, bool& out, std::vector<std::string>* w, const std::string& where) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return false;
    if (it->is_boolean()) {
        out = it->get<bool>();
        return true;
    }
    if (it->is_number()) {
        out = it->get<double>() != 0.0;
        return true;
    }
    warn(w, where + "." + key + ": expected a boolean, ignored");
    return false;
}

bool readStr(const json& j, const char* key, std::string& out, std::vector<std::string>* w, const std::string& where) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return false;
    if (it->is_string()) {
        out = it->get<std::string>();
        return true;
    }
    warn(w, where + "." + key + ": expected a string, ignored");
    return false;
}

NodeId readNode(const json& j) {
    auto it = j.find("node");
    if (it != j.end() && it->is_number_unsigned()) {
        const uint64_t v = it->get<uint64_t>();
        if (v > 0 && v < 0xFFFFFFFFull) return static_cast<NodeId>(v);
    }
    if (it != j.end() && it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        if (v > 0 && v < 0xFFFFFFFFll) return static_cast<NodeId>(v);
    }
    return 0;
}

void unknownKeys(const json& j, std::initializer_list<const char*> known, std::vector<std::string>* w,
                 const std::string& where) {
    if (!w || !j.is_object()) return;
    std::set<std::string> k(known.begin(), known.end());
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!k.count(it.key())) w->push_back(where + ": unknown key '" + it.key() + "' ignored");
}

std::vector<ModuleSlot> fxFromJson(const json& j, std::vector<std::string>* w, const std::string& where) {
    std::vector<ModuleSlot> out;
    if (j.is_null()) return out;
    if (!j.is_array()) {
        warn(w, where + ": expected an array");
        return out;
    }
    int i = 0;
    for (const auto& f : j) {
        const std::string fw = where + "[" + std::to_string(i++) + "]";
        if (!f.is_object()) {
            warn(w, fw + ": expected an object, ignored");
            continue;
        }
        out.push_back(slotFromJson(f, w, fw));
    }
    return out;
}

} // namespace

Zone zoneFromJson(const json& j, const Zone& base, std::vector<std::string>* w) {
    Zone z = base;
    if (!j.is_object()) return z;
    const std::string where = "zone";
    readNum(j, "key_lo", z.keyLo, w, where);
    readNum(j, "key_hi", z.keyHi, w, where);
    readNum(j, "vel_lo", z.velLo, w, where);
    readNum(j, "vel_hi", z.velHi, w, where);
    readNum(j, "transpose", z.transpose, w, where);
    readNum(j, "channel", z.channel, w, where);
    readNum(j, "volume_db", z.volumeDb, w, where);
    readNum(j, "pan", z.pan, w, where);
    readBool(j, "mute", z.mute, w, where);
    readBool(j, "solo", z.solo, w, where);
    readBool(j, "sustain", z.sustain, w, where);
    unknownKeys(j, {"key_lo", "key_hi", "vel_lo", "vel_hi", "transpose", "channel", "volume_db", "pan", "mute", "solo",
                    "sustain"},
                w, where);
    return z;
}

json zoneToJson(const Zone& z) {
    return {{"key_lo", z.keyLo},       {"key_hi", z.keyHi}, {"vel_lo", z.velLo},     {"vel_hi", z.velHi},
            {"transpose", z.transpose}, {"channel", z.channel}, {"volume_db", z.volumeDb}, {"pan", z.pan},
            {"mute", z.mute},           {"solo", z.solo},     {"sustain", z.sustain}};
}

ModuleSlot slotFromJson(const json& j, std::vector<std::string>* w, const std::string& where) {
    ModuleSlot s;
    if (!j.is_object()) return s;
    s.node = readNode(j);
    readStr(j, "type", s.type, w, where);
    readBool(j, "bypass", s.bypass, w, where);
    if (auto it = j.find("params"); it != j.end() && it->is_object()) {
        for (auto p = it->begin(); p != it->end(); ++p) {
            if (p->is_number() && std::isfinite(p->get<double>())) s.params[p.key()] = p->get<float>();
            else if (p->is_boolean()) s.params[p.key()] = p->get<bool>() ? 1.0f : 0.0f;
            else warn(w, where + ".params." + p.key() + ": not a number, ignored");
        }
    }
    if (auto it = j.find("state"); it != j.end() && it->is_object()) s.state = *it;
    unknownKeys(j, {"node", "type", "bypass", "params", "state"}, w, where);
    return s;
}

json slotToJson(const ModuleSlot& s, bool isFx) {
    json params = json::object();
    for (const auto& [k, v] : s.params) params[k] = v;
    json j = {{"node", s.node}, {"type", s.type}};
    if (isFx) j["bypass"] = s.bypass;
    j["params"] = params;
    j["state"] = s.state.is_object() ? s.state : json::object();
    return j;
}

Patch patchFromJson(const json& in, std::vector<std::string>* w) {
    Patch p;
    p.layers.clear();
    if (!in.is_object()) {
        warn(w, "patch: expected an object");
        return p;
    }
    json j = migratePatchJson(in, w);
    readNum(j, "format", p.format, w, "patch");
    p.format = kPatchFormat;
    if (auto it = j.find("meta"); it != j.end() && it->is_object()) {
        const json& m = *it;
        readStr(m, "name", p.meta.name, w, "meta");
        readStr(m, "category", p.meta.category, w, "meta");
        readStr(m, "description", p.meta.description, w, "meta");
        readStr(m, "author", p.meta.author, w, "meta");
        if (auto t = m.find("tags"); t != m.end() && t->is_array())
            for (const auto& tag : *t)
                if (tag.is_string()) p.meta.tags.push_back(tag.get<std::string>());
        unknownKeys(m, {"name", "category", "tags", "description", "author"}, w, "meta");
    }
    readNum(j, "tempo", p.tempo, w, "patch");
    if (auto it = j.find("layers"); it != j.end()) {
        if (!it->is_array()) {
            warn(w, "layers: expected an array");
        } else {
            int i = 0;
            for (const auto& lj : *it) {
                const std::string where = "layers[" + std::to_string(i++) + "]";
                if (!lj.is_object()) {
                    warn(w, where + ": expected an object, ignored");
                    continue;
                }
                Layer l;
                l.node = readNode(lj);
                readStr(lj, "name", l.name, w, where);
                if (auto z = lj.find("zone"); z != lj.end()) l.zone = zoneFromJson(*z, Zone{}, w);
                if (auto ins = lj.find("instrument"); ins != lj.end() && ins->is_object())
                    l.instrument = slotFromJson(*ins, w, where + ".instrument");
                else
                    warn(w, where + ": missing instrument");
                if (auto fx = lj.find("fx"); fx != lj.end()) l.fx = fxFromJson(*fx, w, where + ".fx");
                unknownKeys(lj, {"node", "name", "zone", "instrument", "fx"}, w, where);
                p.layers.push_back(std::move(l));
            }
        }
    }
    if (auto it = j.find("master"); it != j.end() && it->is_object()) {
        readNum(*it, "volume_db", p.master.volumeDb, w, "master");
        if (auto fx = it->find("fx"); fx != it->end()) p.master.fx = fxFromJson(*fx, w, "master.fx");
        unknownKeys(*it, {"volume_db", "fx", "node"}, w, "master");
    }
    if (auto it = j.find("rhythm"); it != j.end() && it->is_object()) p.rhythm = *it;
    unknownKeys(j, {"format", "meta", "tempo", "layers", "master", "rhythm"}, w, "patch");
    return p;
}

json patchToJson(const Patch& p) {
    json layers = json::array();
    for (const auto& l : p.layers) {
        json fx = json::array();
        for (const auto& f : l.fx) fx.push_back(slotToJson(f, true));
        layers.push_back({{"node", l.node},
                          {"name", l.name},
                          {"zone", zoneToJson(l.zone)},
                          {"instrument", slotToJson(l.instrument, false)},
                          {"fx", fx}});
    }
    json mfx = json::array();
    for (const auto& f : p.master.fx) mfx.push_back(slotToJson(f, true));
    json j = {{"format", kPatchFormat},
              {"meta",
               {{"name", p.meta.name},
                {"category", p.meta.category},
                {"tags", p.meta.tags},
                {"description", p.meta.description},
                {"author", p.meta.author}}},
              {"tempo", p.tempo},
              {"layers", layers},
              {"master", {{"volume_db", p.master.volumeDb}, {"fx", mfx}}}};
    if (p.rhythm.is_object()) j["rhythm"] = p.rhythm;
    return j;
}

} // namespace ks
