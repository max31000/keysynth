#include "control/ProtocolHandler.h"

#include "plugins/PluginHost.h"
#include "preset/PatchJson.h"

#include <cmath>
#include <functional>
#include <map>

namespace ks {

using nlohmann::json;

namespace {

// Field accessors: throw PatchError("bad_request") on missing/wrong-typed fields.
const json& field(const json& m, const char* key) {
    auto it = m.find(key);
    if (it == m.end() || it->is_null()) throw PatchError("bad_request", std::string("missing field '") + key + "'");
    return *it;
}

NodeId nodeField(const json& m, const char* key) {
    const json& v = field(m, key);
    if (!v.is_number_integer() && !v.is_number_unsigned())
        throw PatchError("bad_request", std::string("field '") + key + "' must be a non-negative integer");
    const int64_t i = v.get<int64_t>();
    if (i < 0 || i > 0xFFFFFFFFll) throw PatchError("bad_request", std::string("field '") + key + "' out of range");
    return static_cast<NodeId>(i);
}

std::string strField(const json& m, const char* key) {
    const json& v = field(m, key);
    if (!v.is_string()) throw PatchError("bad_request", std::string("field '") + key + "' must be a string");
    return v.get<std::string>();
}

double numField(const json& m, const char* key) {
    const json& v = field(m, key);
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    if (!v.is_number()) throw PatchError("bad_request", std::string("field '") + key + "' must be a number");
    const double d = v.get<double>();
    if (!std::isfinite(d)) throw PatchError("bad_request", std::string("field '") + key + "' must be finite");
    return d;
}

bool boolField(const json& m, const char* key) {
    const json& v = field(m, key);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    throw PatchError("bad_request", std::string("field '") + key + "' must be a boolean");
}

std::optional<int> optInt(const json& m, const char* key) {
    auto it = m.find(key);
    if (it == m.end() || it->is_null()) return std::nullopt;
    if (!it->is_number()) throw PatchError("bad_request", std::string("field '") + key + "' must be a number");
    return static_cast<int>(it->get<double>());
}

int midiChannel(const json& m) {
    auto c = optInt(m, "channel");
    if (!c || *c <= 0) return 1;
    if (*c > 16) throw PatchError("bad_request", "channel must be 1..16");
    return *c;
}

int u7(double v, const char* what) {
    if (v < 0 || v > 127) throw PatchError("bad_request", std::string(what) + " must be 0..127");
    return static_cast<int>(std::lround(v));
}

struct Ctx {
    Session& s;
    const json& msg;
    const json& id;
    std::vector<Outgoing>& out;

    void reply(json m) {
        if (!id.is_null()) m["id"] = id;
        out.push_back({Outgoing::Target::Reply, std::move(m)});
    }
    void others(json m) { out.push_back({Outgoing::Target::Others, std::move(m)}); }
    void broadcast(json m) { out.push_back({Outgoing::Target::Broadcast, std::move(m)}); }
    // Structural change: requester gets `state` with id, everyone else a plain `state`.
    void stateToAll() {
        json st = s.stateJson();
        others(st);
        reply(std::move(st));
    }
};

using Handler = std::function<void(Ctx&)>;

const std::map<std::string, Handler>& handlers() {
    static const std::map<std::string, Handler> h = {
        {"hello", [](Ctx& c) { c.reply(c.s.stateJson()); }},
        {"get_catalog",
         [](Ctx& c) { c.reply({{"type", "catalog_ok"}, {"modules", c.s.registry().catalogJson()}}); }},
        {"set_param",
         [](Ctx& c) {
             const NodeId node = nodeField(c.msg, "node");
             const std::string param = strField(c.msg, "param");
             const float v = c.s.setParam(node, param, static_cast<float>(numField(c.msg, "value")));
             c.others({{"type", "param"}, {"node", node}, {"param", param}, {"value", v}});
         }},
        {"load_preset",
         [](Ctx& c) {
             const auto warnings = c.s.loadPreset(strField(c.msg, "path"));
             c.reply({{"type", "load_preset_ok"}, {"path", c.s.presetPath()}, {"warnings", warnings}});
             c.broadcast(c.s.stateJson());
         }},
        {"save_preset",
         [](Ctx& c) {
             std::string category;
             if (auto it = c.msg.find("category"); it != c.msg.end() && it->is_string()) category = it->get<std::string>();
             bool overwrite = false;
             if (auto it = c.msg.find("overwrite"); it != c.msg.end() && it->is_boolean()) overwrite = it->get<bool>();
             const std::string path = c.s.savePreset(strField(c.msg, "name"), category, overwrite);
             c.reply({{"type", "save_preset_ok"}, {"path", path}});
             c.others(c.s.stateJson());
         }},
        {"list_presets",
         [](Ctx& c) {
             json arr = json::array();
             for (const auto& p : c.s.presets().list())
                 arr.push_back({{"path", p.path}, {"name", p.name}, {"category", p.category}, {"tags", p.tags}, {"factory", p.factory}});
             c.reply({{"type", "list_presets_ok"}, {"presets", arr}});
         }},
        {"get_patch", [](Ctx& c) { c.reply(c.s.stateJson()); }},
        {"set_patch",
         [](Ctx& c) {
             const json& pj = field(c.msg, "patch");
             if (!pj.is_object()) throw PatchError("bad_request", "patch must be an object");
             std::vector<std::string> w;
             Patch p = patchFromJson(pj, &w);
             // Reuse modules only for an edit of the current patch (every layer id already exists).
             bool related = !p.layers.empty();
             for (const auto& l : p.layers) related = related && l.node != 0 && c.s.model().findLayer(l.node) != nullptr;
             c.s.setPatch(std::move(p), c.s.presetPath(), related);
             c.s.model().setDirty(true);
             c.stateToAll();
         }},
        {"add_layer",
         [](Ctx& c) {
             std::string type = "basic";
             if (auto it = c.msg.find("module"); it != c.msg.end() && it->is_string()) type = it->get<std::string>();
             c.s.model().addLayer(optInt(c.msg, "index"), type);
             c.s.rebuild();
             c.stateToAll();
         }},
        {"remove_layer",
         [](Ctx& c) {
             c.s.model().removeLayer(nodeField(c.msg, "layer"));
             c.s.rebuild();
             c.stateToAll();
         }},
        {"set_zone",
         [](Ctx& c) {
             const NodeId layer = nodeField(c.msg, "layer");
             const json& zj = field(c.msg, "zone");
             if (!zj.is_object()) throw PatchError("bad_request", "zone must be an object");
             const Layer* l = c.s.model().findLayer(layer);
             if (!l) throw PatchError("not_found", "no layer " + std::to_string(layer));
             c.s.setZone(layer, zoneFromJson(zj, l->zone));
         }},
        {"set_instrument",
         [](Ctx& c) {
             c.s.model().setInstrument(nodeField(c.msg, "layer"), strField(c.msg, "module"));
             c.s.rebuild();
             c.stateToAll();
         }},
        {"add_fx",
         [](Ctx& c) {
             c.s.model().addFx(nodeField(c.msg, "layer"), strField(c.msg, "module"), optInt(c.msg, "index"));
             c.s.rebuild();
             c.stateToAll();
         }},
        {"remove_fx",
         [](Ctx& c) {
             c.s.model().removeFx(nodeField(c.msg, "node"));
             c.s.rebuild();
             c.stateToAll();
         }},
        {"move_fx",
         [](Ctx& c) {
             auto to = optInt(c.msg, "to");
             if (!to) throw PatchError("bad_request", "missing field 'to'");
             c.s.model().moveFx(nodeField(c.msg, "node"), *to);
             c.s.rebuild();
             c.stateToAll();
         }},
        {"set_fx_bypass",
         [](Ctx& c) { c.s.setFxBypass(nodeField(c.msg, "node"), boolField(c.msg, "bypass")); }},
        {"rescan_midi",
         [](Ctx& c) {
             if (c.s.midi()) c.s.midi()->rescan();
             c.reply(c.s.devicesJson());
         }},
        {"note",
         [](Ctx& c) {
             const bool on = boolField(c.msg, "on");
             const int note = u7(numField(c.msg, "note"), "note");
             int vel = 100;
             if (auto it = c.msg.find("velocity"); it != c.msg.end() && !it->is_null()) vel = u7(numField(c.msg, "velocity"), "velocity");
             const int ch = midiChannel(c.msg);
             const MidiEvent e = on && vel > 0 ? MidiEvent::noteOn(note, vel, ch) : MidiEvent::noteOff(note, 0, ch);
             if (!c.s.engine().pushEvent(e)) throw PatchError("busy", "event queue full");
         }},
        {"cc",
         [](Ctx& c) {
             const int cc = u7(numField(c.msg, "cc"), "cc");
             const int value = u7(numField(c.msg, "value"), "value");
             const uint8_t bytes[3] = {static_cast<uint8_t>(0xB0 | (midiChannel(c.msg) - 1)), static_cast<uint8_t>(cc),
                                       static_cast<uint8_t>(value)};
             MidiEvent e;
             MidiEvent::fromBytes(bytes, 3, e); // same mapping as hardware (CC120/123 -> sound/notes off)
             if (!c.s.engine().pushEvent(e))
                 throw PatchError("busy", "event queue full");
         }},
        {"transport",
         [](Ctx& c) {
             const json& m = c.msg;
             if (m.contains("pattern") || m.contains("drums"))
                 throw PatchError("not_implemented", "drum sequencer is not implemented yet");
             bool any = false;
             if (m.contains("tempo")) { c.s.setTempo(numField(m, "tempo")); any = true; }
             if (m.contains("playing")) { c.s.engine().transport().setPlaying(boolField(m, "playing")); any = true; }
             if (m.contains("metronome")) { c.s.engine().metronome().setEnabled(boolField(m, "metronome")); any = true; }
             if (m.contains("metronome_volume")) {
                 c.s.engine().metronome().setVolume(static_cast<float>(numField(m, "metronome_volume")));
                 any = true;
             }
             if (any) c.others(c.s.stateJson()); // keep other clients' transport view in sync
         }},
        {"list_devices", [](Ctx& c) { c.reply(c.s.devicesJson()); }},
        {"set_audio_device",
         [](Ctx& c) {
             if (!c.s.audio()) throw PatchError("not_available", "no audio host");
             double sr = 0;
             int bs = 0;
             if (c.msg.contains("sample_rate")) sr = numField(c.msg, "sample_rate");
             if (c.msg.contains("buffer_size")) bs = static_cast<int>(numField(c.msg, "buffer_size"));
             const std::string err = c.s.audio()->setDevice(strField(c.msg, "device_type"), strField(c.msg, "name"), sr, bs);
             if (!err.empty()) throw PatchError("device_error", err);
             json d = c.s.devicesJson();
             c.others(d);
             c.reply(std::move(d));
         }},
        {"panic", [](Ctx& c) { c.s.engine().panic(); }},
        {"list_plugins",
         [](Ctx& c) {
             json arr = json::array();
             if (c.s.plugins())
                 for (const auto& st : c.s.plugins()->list()) arr.push_back(plugins::toJson(st));
             c.reply({{"type", "list_plugins_ok"}, {"plugins", arr}, {"faust", plugins::PluginHost::faustInfo()}});
         }},
        {"reload_plugin",
         [](Ctx& c) {
             if (!c.s.plugins()) throw PatchError("not_available", "plugin host not running");
             const std::string name = strField(c.msg, "name");
             if (!plugins::PluginHost::isValidName(name)) throw PatchError("bad_request", "invalid plugin name");
             std::string err;
             if (!c.s.plugins()->reload(name, err)) throw PatchError("not_found", err);
             // Progress/result arrive as plugin_status events.
             c.reply({{"type", "reload_plugin_ok"}, {"name", name}});
         }},
    };
    return h;
}

} // namespace

json ProtocolHandler::error(const json& id, const std::string& code, const std::string& message) {
    json e = {{"type", "error"}, {"code", code}, {"message", message}};
    if (!id.is_null()) e["id"] = id;
    return e;
}

std::vector<Outgoing> ProtocolHandler::handleText(const std::string& text) {
    json msg = json::parse(text, nullptr, false);
    if (msg.is_discarded()) return {{Outgoing::Target::Reply, error(nullptr, "parse_error", "invalid JSON")}};
    return handle(msg);
}

std::vector<Outgoing> ProtocolHandler::handle(const json& msg) {
    std::vector<Outgoing> out;
    if (!msg.is_object()) return {{Outgoing::Target::Reply, error(nullptr, "bad_request", "message must be an object")}};
    json id = nullptr;
    if (auto it = msg.find("id"); it != msg.end() && it->is_number()) id = *it;
    auto t = msg.find("type");
    if (t == msg.end() || !t->is_string()) return {{Outgoing::Target::Reply, error(id, "bad_request", "missing 'type'")}};
    const std::string type = t->get<std::string>();
    const auto& h = handlers();
    auto it = h.find(type);
    if (it == h.end()) return {{Outgoing::Target::Reply, error(id, "unknown_type", "unknown message type '" + type + "'")}};
    Ctx c{session_, msg, id, out};
    try {
        it->second(c);
    } catch (const PatchError& e) {
        out.clear();
        out.push_back({Outgoing::Target::Reply, error(id, e.code(), e.what())});
    } catch (const std::exception& e) {
        out.clear();
        out.push_back({Outgoing::Target::Reply, error(id, "internal", e.what())});
    }
    return out;
}

} // namespace ks
