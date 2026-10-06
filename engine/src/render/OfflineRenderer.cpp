#include "render/OfflineRenderer.h"

#include "core/AppPaths.h"
#include "core/Engine.h"
#include "core/GraphBuilder.h"
#include "core/PatchModel.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace ks {

RenderResult OfflineRenderer::render(const Patch& patch, std::vector<TimedEvent> events, const RenderOptions& opt,
                                     const ModuleRegistry& registry) {
    RenderResult r;
    r.sampleRate = opt.sampleRate;
    const int block = std::clamp(opt.blockSize, 1, Engine::kMaxDeviceBlock);

    PatchModel model(registry);
    r.warnings = model.setPatch(patch);

    auto engine = std::make_unique<Engine>();
    engine->prepare(opt.sampleRate, block);
    double patternSeconds = 0.0;
    if (opt.pattern) {
        if (opt.pattern->tempo > 0) model.setTempo(opt.pattern->tempo);
        if (opt.tempo > 0) model.setTempo(opt.tempo);
        if (opt.pattern->kit >= 0 && model.patch().rhythm.drums.node != 0)
            model.setParam(model.patch().rhythm.drums.node, "kit", static_cast<float>(opt.pattern->kit));
        engine->sequencer().setPattern(toRt(*opt.pattern));
        engine->transport().setTimeSignature(opt.pattern->numerator, opt.pattern->denominator);
        engine->rhythm().swing.store(opt.pattern->swing);
        patternSeconds = std::max(1, opt.patternBars) * opt.pattern->barPpq() * 60.0 / model.patch().tempo;
    } else if (opt.tempo > 0) {
        model.setTempo(opt.tempo);
    }
    engine->transport().setTempo(model.patch().tempo);
    if (opt.pattern) engine->transport().setPlaying(true);
    BuildResult b = GraphBuilder::build(model.patch(), registry, nullptr, opt.sampleRate, block);
    r.warnings.insert(r.warnings.end(), b.warnings.begin(), b.warnings.end());
    // Offline: modules may block on IO in process(); wait until asynchronous loads (samples) have finished.
    if (b.graph) {
        const auto nodes = b.graph->allModuleNodes();
        for (ModuleNode* n : nodes)
            if (n->module) n->module->setOfflineMode(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt.readyTimeoutSeconds);
        for (ModuleNode* n : nodes) {
            while (n->module && !n->module->isReady()) {
                if (std::chrono::steady_clock::now() > deadline) {
                    r.warnings.push_back("module '" + n->type + "' not ready after timeout; rendering anyway");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        for (ModuleNode* n : nodes)
            if (n->module)
                if (const std::string err = n->module->loadError(); !err.empty())
                    r.warnings.push_back("module '" + n->type + "' (node " + std::to_string(n->node) + "): " + err);
    }
    engine->publish(std::move(b.graph));

    std::stable_sort(events.begin(), events.end(), [](const TimedEvent& a, const TimedEvent& b2) { return a.time < b2.time; });
    double lastTime = 0.0;
    for (const auto& e : events) lastTime = std::max(lastTime, e.time);
    lastTime = std::max(lastTime, patternSeconds);
    const int64_t stopAt = static_cast<int64_t>(std::llround(patternSeconds * opt.sampleRate));
    const int64_t total = static_cast<int64_t>(std::ceil((lastTime + std::max(0.0, opt.tailSeconds)) * opt.sampleRate));
    r.left.assign(static_cast<size_t>(total), 0.0f);
    r.right.assign(static_cast<size_t>(total), 0.0f);

    std::vector<float> l(static_cast<size_t>(block)), rr(static_cast<size_t>(block));
    std::vector<MidiEvent> blockEvents;
    blockEvents.reserve(kMaxEventsPerBlock);
    size_t ei = 0;
    int64_t blockIndex = 0;
    for (int64_t pos = 0; pos < total; ++blockIndex) {
        int n = static_cast<int>(std::min<int64_t>(block, total - pos));
        if (opt.pattern && pos < stopAt) n = static_cast<int>(std::min<int64_t>(n, stopAt - pos)); // stop exactly
        if (opt.pattern && pos == stopAt) engine->transport().setPlaying(false);
        blockEvents.clear();
        while (ei < events.size()) {
            const int64_t at = static_cast<int64_t>(std::llround(events[ei].time * opt.sampleRate));
            if (at >= pos + n) break;
            MidiEvent e = events[ei].event;
            e.sampleOffset = static_cast<uint32_t>(std::clamp<int64_t>(at - pos, 0, n - 1));
            if (blockEvents.size() < static_cast<size_t>(kMaxEventsPerBlock)) blockEvents.push_back(e);
            ++ei;
        }
        AudioBlock ab{l.data(), rr.data(), n};
        engine->processBlock(ab, MidiEventSpan(blockEvents.data(), blockEvents.size()));
        std::memcpy(r.left.data() + pos, l.data(), sizeof(float) * static_cast<size_t>(n));
        std::memcpy(r.right.data() + pos, rr.data(), sizeof(float) * static_cast<size_t>(n));
        if (blockIndex % 64 == 0) engine->collectGarbage();
        pos += n;
    }
    engine->collectGarbage();
    return r;
}

std::vector<TimedEvent> OfflineRenderer::notesToEvents(const std::vector<NoteSpec>& notes, int channel) {
    std::vector<TimedEvent> out;
    for (const auto& n : notes) {
        out.push_back({n.start, MidiEvent::noteOn(n.note, std::clamp(n.velocity, 1, 127), channel)});
        out.push_back({n.start + std::max(0.0, n.duration), MidiEvent::noteOff(n.note, 0, channel)});
    }
    std::stable_sort(out.begin(), out.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.time < b.time; });
    return out;
}

int OfflineRenderer::parseNoteName(const std::string& sIn) {
    std::string s;
    for (char c : sIn)
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    if (s.empty()) return -1;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
        try {
            size_t used = 0;
            const int v = std::stoi(s, &used);
            return used == s.size() && v >= 0 && v <= 127 ? v : -1;
        } catch (...) {
            return -1;
        }
    }
    static const int base[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char L = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (L < 'A' || L > 'G') return -1;
    int note = base[L - 'A'];
    size_t i = 1;
    while (i < s.size() && (s[i] == '#' || s[i] == 'b')) note += s[i++] == '#' ? 1 : -1;
    if (i >= s.size()) return -1;
    try {
        size_t used = 0;
        const int octave = std::stoi(s.substr(i), &used);
        if (used != s.size() - i) return -1;
        const int v = (octave + 1) * 12 + note;
        return v >= 0 && v <= 127 ? v : -1;
    } catch (...) {
        return -1;
    }
}

bool OfflineRenderer::parseNotes(const std::string& spec, std::vector<NoteSpec>& out, std::string& error) {
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.find_first_not_of(" \t") == std::string::npos) continue;
        std::vector<std::string> parts;
        std::stringstream is(item);
        std::string p;
        while (std::getline(is, p, ':')) parts.push_back(p);
        if (parts.size() < 3 || parts.size() > 4) {
            error = "bad note spec '" + item + "' (expected NOTE:start:dur[:vel])";
            return false;
        }
        NoteSpec n;
        n.note = parseNoteName(parts[0]);
        if (n.note < 0) {
            error = "bad note name '" + parts[0] + "'";
            return false;
        }
        try {
            n.start = std::stod(parts[1]);
            n.duration = std::stod(parts[2]);
            if (parts.size() == 4) n.velocity = std::stoi(parts[3]);
        } catch (...) {
            error = "bad number in '" + item + "'";
            return false;
        }
        if (n.start < 0 || n.duration < 0 || n.velocity < 1 || n.velocity > 127) {
            error = "out-of-range value in '" + item + "'";
            return false;
        }
        out.push_back(n);
    }
    return true;
}

std::vector<TimedEvent> OfflineRenderer::standardTestEvents() {
    std::vector<NoteSpec> notes;
    // Chord (C major, mf) 0..1 s
    for (int n : {48, 60, 64, 67, 72}) notes.push_back({n, 0.0, 1.0, 100});
    // Scale C4..C5, 0.15 s per note, from 1.2 s
    const int scale[] = {60, 62, 64, 65, 67, 69, 71, 72};
    for (int i = 0; i < 8; ++i) notes.push_back({scale[i], 1.2 + 0.15 * i, 0.14, 70 + i * 6});
    // Low / high extremes
    notes.push_back({24, 2.5, 0.3, 110});
    notes.push_back({96, 2.5, 0.3, 110});
    auto ev = notesToEvents(notes);
    // Sustain pedal section: pedal down 3.0 s, short notes, pedal up 3.8 s
    ev.push_back({3.0, MidiEvent::cc(64, 127)});
    for (int n : {57, 62, 65}) {
        ev.push_back({3.05, MidiEvent::noteOn(n, 90)});
        ev.push_back({3.25, MidiEvent::noteOff(n)});
    }
    ev.push_back({3.8, MidiEvent::cc(64, 0)});
    std::stable_sort(ev.begin(), ev.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.time < b.time; });
    return ev;
}

bool OfflineRenderer::writeWav(const std::string& path, const RenderResult& r, std::string& error) {
    std::ofstream f(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    const uint32_t frames = static_cast<uint32_t>(r.left.size());
    const uint16_t channels = 2, bits = 32, format = 3; // IEEE float
    const uint32_t sr = static_cast<uint32_t>(std::llround(r.sampleRate));
    const uint32_t dataBytes = frames * channels * (bits / 8);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(18);
    u16(format);
    u16(channels);
    u32(sr);
    u32(sr * channels * (bits / 8));
    u16(static_cast<uint16_t>(channels * (bits / 8)));
    u16(bits);
    u16(0); // cbSize
    f.write("fact", 4);
    u32(4);
    u32(frames);
    f.write("data", 4);
    u32(dataBytes);
    std::vector<float> inter(static_cast<size_t>(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        inter[2 * i] = r.left[i];
        inter[2 * i + 1] = r.right[i];
    }
    f.write(reinterpret_cast<const char*>(inter.data()), static_cast<std::streamsize>(inter.size() * sizeof(float)));
    if (!f) {
        error = "write failed: " + path;
        return false;
    }
    return true;
}

} // namespace ks
