// ks-render: offline render of a patch to WAV (docs/TESTING.md).
//   ks-render --preset presets/factory/synth-lead/basic-saw-lead.json --notes "C4:0:1,E4:0:1,G4:0:1" --out x.wav
//   ks-render --patch-json '{...}' | --patch-json @file.json   --midi song.mid   --sr 48000 --block 64 --tail 2
//   ks-render --test-pattern ...      (standard chord/scale/sustain pattern used by the render tests)
//   ks-render --list-modules          (catalog JSON)

#include "core/AppPaths.h"
#include "core/ModuleRegistry.h"
#include "core/PatchModel.h"
#include "core/RtCheck.h"
#include "preset/PatchJson.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
int usage(const char* err = nullptr) {
    if (err) std::fprintf(stderr, "error: %s\n", err);
    std::fprintf(stderr,
                 "usage: ks-render (--preset PATH | --patch-json JSON|@FILE) [--notes SPEC | --midi FILE | --test-pattern]\n"
                 "                 [--sr HZ] [--block N] [--tail SEC] [--out FILE.wav]\n"
                 "       ks-render --list-modules\n"
                 "  SPEC: NOTE:start:dur[:vel],...  e.g. \"C4:0:1:100,E4:0.5:1\" (seconds; C4 = 60)\n");
    return 2;
}
} // namespace

int main(int argc, char** argv) {
    std::string preset, patchJson, notes, midi, out = "renders/out.wav";
    double sr = 48000.0, tail = 2.0;
    int block = 64;
    bool listModules = false, testPattern = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        try {
            if (a == "--preset") preset = next();
            else if (a == "--patch-json") patchJson = next();
            else if (a == "--notes") notes = next();
            else if (a == "--midi") midi = next();
            else if (a == "--sr") sr = std::stod(next());
            else if (a == "--block") block = std::stoi(next());
            else if (a == "--tail") tail = std::stod(next());
            else if (a == "--out") out = next();
            else if (a == "--list-modules") listModules = true;
            else if (a == "--test-pattern") testPattern = true;
            else if (a == "--help" || a == "-h") return usage();
            else return usage(("unknown argument " + a).c_str());
        } catch (...) {
            return usage(("bad value for " + a).c_str());
        }
    }
    auto& registry = ks::defaultRegistry();
    if (listModules) {
        std::cout << registry.catalogJson().dump(2) << std::endl;
        return 0;
    }
    if (sr < 8000 || sr > 384000 || block < 1 || block > 8192 || tail < 0) return usage("out-of-range --sr/--block/--tail");

    ks::Patch patch;
    std::vector<std::string> warnings;
    try {
        if (!preset.empty()) {
            patch = ks::PresetStore::loadFile(preset, &warnings);
        } else if (!patchJson.empty()) {
            std::string text = patchJson;
            if (text[0] == '@') {
                std::ifstream f(ks::pathFromUtf8(text.substr(1)), std::ios::binary);
                if (!f) return usage(("cannot open " + text.substr(1)).c_str());
                std::stringstream ss;
                ss << f.rdbuf();
                text = ss.str();
            }
            patch = ks::patchFromJson(nlohmann::json::parse(text), &warnings);
        } else {
            patch = ks::PatchModel::makeDefaultPatch();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: cannot load patch: %s\n", e.what());
        return 1;
    }

    std::vector<ks::TimedEvent> events;
    std::string err;
    if (!midi.empty()) {
        if (!ks::OfflineRenderer::loadMidiFile(midi, events, err)) return usage(err.c_str());
    } else if (!notes.empty()) {
        std::vector<ks::NoteSpec> specs;
        if (!ks::OfflineRenderer::parseNotes(notes, specs, err)) return usage(err.c_str());
        events = ks::OfflineRenderer::notesToEvents(specs);
    } else if (testPattern) {
        events = ks::OfflineRenderer::standardTestEvents();
    } else {
        events = ks::OfflineRenderer::notesToEvents({{60, 0.0, 1.0, 100}});
    }

    ks::RenderOptions opt;
    opt.sampleRate = sr;
    opt.blockSize = block;
    opt.tailSeconds = tail;
    ks::RenderResult r = ks::OfflineRenderer::render(patch, events, opt, registry);
    for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    for (const auto& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());

    std::error_code ec;
    const auto outPath = ks::pathFromUtf8(out);
    if (outPath.has_parent_path()) std::filesystem::create_directories(outPath.parent_path(), ec);
    if (!ks::OfflineRenderer::writeWav(out, r, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    float peak = 0.0f;
    for (size_t i = 0; i < r.left.size(); ++i) peak = std::max({peak, std::fabs(r.left[i]), std::fabs(r.right[i])});
    std::printf("wrote %s: %.2f s @ %.0f Hz, block %d, peak %.2f dBFS, rt violations %llu\n", out.c_str(), r.seconds(),
                sr, block, peak > 0 ? 20.0 * std::log10(peak) : -999.0,
                static_cast<unsigned long long>(ks::rt::violationCount()));
    return 0;
}
