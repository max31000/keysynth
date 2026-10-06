#include "plugins/FaustParamMap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace ks::plugins {

std::string toSnakeCase(std::string_view s) {
    std::string out;
    bool pendingSep = false;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            if (pendingSep && !out.empty()) out.push_back('_');
            pendingSep = false;
            out.push_back(static_cast<char>(std::tolower(c)));
        } else {
            pendingSep = true;
        }
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), 'p');
    return out;
}

bool isValidParamId(std::string_view id) {
    if (id.empty() || id.size() > 64 || !(id[0] >= 'a' && id[0] <= 'z')) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool parseFaustMenu(std::string_view style, std::vector<std::string>& labels, std::vector<float>& values) {
    labels.clear();
    values.clear();
    const size_t open = style.find('{'), close = style.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return false;
    std::string_view body = style.substr(open + 1, close - open - 1);
    size_t i = 0;
    auto skipWs = [&] {
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    };
    while (true) {
        skipWs();
        if (i >= body.size()) break;
        if (body[i] != '\'') return false;
        const size_t end = body.find('\'', i + 1);
        if (end == std::string_view::npos) return false;
        std::string label(body.substr(i + 1, end - i - 1));
        i = end + 1;
        skipWs();
        if (i >= body.size() || body[i] != ':') return false;
        ++i;
        const size_t stop = body.find(';', i);
        std::string num(body.substr(i, stop == std::string_view::npos ? std::string_view::npos : stop - i));
        try {
            size_t used = 0;
            const float v = std::stof(num, &used);
            labels.push_back(std::move(label));
            values.push_back(v);
        } catch (...) {
            return false;
        }
        if (stop == std::string_view::npos) break;
        i = stop + 1;
    }
    return !labels.empty();
}

namespace {

VoiceRole voiceRoleFor(const std::string& label) {
    const std::string l = toSnakeCase(label);
    if (l == "freq") return VoiceRole::Freq;
    if (l == "key") return VoiceRole::Key;
    if (l == "gain") return VoiceRole::Gain;
    if (l == "vel" || l == "velocity") return VoiceRole::Velocity;
    if (l == "gate") return VoiceRole::Gate;
    return VoiceRole::None;
}

std::string metaOr(const FaustControl& c, const char* key, const std::string& def = {}) {
    auto it = c.meta.find(key);
    return it == c.meta.end() ? def : it->second;
}

bool isIntegral(float v) { return std::fabs(v - std::round(v)) < 1e-6f; }

std::string displayName(const std::string& label) {
    if (label.empty()) return label;
    std::string s = label;
    for (char& c : s)
        if (c == '_') c = ' ';
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

} // namespace

FaustMapping mapFaustUi(const FaustUiDesc& ui, bool instrument) {
    FaustMapping m;
    if (ui.hasSoundfile) {
        m.error = "soundfile primitives are not supported";
        return m;
    }
    std::map<std::string, std::string> idToPath;
    std::set<std::string> groupsSeen;
    for (const FaustControl& c : ui.controls) {
        FaustBinding b;
        std::string path;
        for (const auto& g : c.groups) path += g + "/";
        path += c.label;

        if (instrument) {
            b.role = voiceRoleFor(c.label);
            if (b.role != VoiceRole::None) {
                m.hasGate |= b.role == VoiceRole::Gate;
                m.hasFreq |= b.role == VoiceRole::Freq || b.role == VoiceRole::Key;
                m.bindings.push_back(std::move(b));
                continue;
            }
        }

        ParamSpec p;
        std::string id = metaOr(c, "id");
        if (!id.empty()) {
            if (!isValidParamId(id)) {
                m.error = "invalid [id:" + id + "] on '" + path + "' (expected [a-z][a-z0-9_]*)";
                return m;
            }
        } else {
            for (const auto& g : c.groups) {
                const std::string sg = toSnakeCase(g);
                if (!sg.empty()) id += sg + "_";
            }
            id += toSnakeCase(c.label);
            if (!isValidParamId(id)) {
                m.error = "cannot derive a param id from '" + path + "'; add [id:some_name] metadata";
                return m;
            }
        }
        if (auto it = idToPath.find(id); it != idToPath.end()) {
            m.error = "duplicate param id '" + id + "' ('" + it->second + "' and '" + path +
                      "'); add [id:...] metadata to one of them";
            return m;
        }
        idToPath[id] = path;

        p.id = id;
        p.name = displayName(c.label.empty() ? id : c.label);
        p.group = c.groups.empty() ? std::string() : c.groups.back();
        if (!p.group.empty() && groupsSeen.insert(p.group).second) m.groupOrder.push_back(p.group);
        p.unit = metaOr(c, "unit");
        if (metaOr(c, "hidden") == "1") p.flags |= ParamFlags::Hidden;

        using K = FaustControl::Kind;
        switch (c.kind) {
        case K::Button:
        case K::CheckBox:
            p.scale = ParamScale::Bool;
            p.min = 0.0f;
            p.max = 1.0f;
            p.def = c.kind == K::CheckBox ? (c.init >= 0.5f ? 1.0f : 0.0f) : 0.0f;
            if (c.kind == K::Button) p.flags |= ParamFlags::NonAutomatable;
            break;
        case K::HBargraph:
        case K::VBargraph:
            p.flags |= ParamFlags::ReadOnly;
            b.readOnly = true;
            p.min = std::min(c.min, c.max);
            p.max = std::max(c.min, c.max);
            p.def = p.min;
            break;
        default: {
            p.min = std::min(c.min, c.max);
            p.max = std::max(c.min, c.max);
            p.def = std::clamp(c.init, p.min, p.max);
            const std::string style = metaOr(c, "style");
            std::vector<std::string> labels;
            std::vector<float> values;
            if ((style.rfind("menu", 0) == 0 || style.rfind("radio", 0) == 0) && parseFaustMenu(style, labels, values)) {
                p.scale = ParamScale::Enum;
                p.choices = labels;
                p.min = 0.0f;
                p.max = static_cast<float>(labels.size() - 1);
                size_t best = 0;
                for (size_t i = 1; i < values.size(); ++i)
                    if (std::fabs(values[i] - c.init) < std::fabs(values[best] - c.init)) best = i;
                p.def = static_cast<float>(best);
                b.enumValues = values;
            } else if (metaOr(c, "scale") == "log" && p.min > 0.0f) {
                p.scale = ParamScale::Log;
            } else if (c.step >= 1.0f && isIntegral(c.step) && isIntegral(p.min) && isIntegral(p.max) && isIntegral(p.def)) {
                p.scale = ParamScale::Int;
            } else {
                p.scale = ParamScale::Linear;
            }
            if (p.max <= p.min) p.max = p.min + 1.0f; // degenerate range: keep the spec valid
            break;
        }
        }
        b.paramIndex = static_cast<int>(m.params.size());
        m.params.push_back(std::move(p));
        m.bindings.push_back(std::move(b));
    }
    if (instrument && !m.hasGate) m.error = "instrument has no 'gate' control (freq/gain/gate convention)";
    return m;
}

} // namespace ks::plugins
