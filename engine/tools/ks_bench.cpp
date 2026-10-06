// ks-bench: realtime factor per factory preset (ARCHITECTURE §12: worst preset needs >= 2x at 48 kHz / 64).
//   ks-bench [--sr 48000] [--block 64] [--seconds 10] [--filter substr]
//   ks-bench --latency [--sr 48000] [--block 64]   note-on -> first output sample per instrument engine
// Renders the standard test pattern repeated to fill --seconds, measures wall time.
// realtime factor = audio seconds / wall seconds.

#include "core/AppPaths.h"
#include "core/ModuleRegistry.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#include "instruments/sampler/SamplePaths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace {

// --latency: MIDI note-on (C4, velocity 100) -> first output sample above the noise floor (max |x| before the
// note, at least 1e-6), through the whole graph (OfflineRenderer), per registered instrument engine with its
// default params; `sampler` uses the first factory preset whose library is installed (first layer only).
// Same measurement as engine/tests/test_note_latency.cpp.
int64_t noteLatency(const ks::Patch& patch, int offset, double sr, int block) {
    constexpr int kPreBlocks = 4;
    const int64_t at = static_cast<int64_t>(kPreBlocks) * block + offset;
    ks::RenderOptions opt;
    opt.sampleRate = sr;
    opt.blockSize = block;
    opt.tailSeconds = 0.05;
    const double t = static_cast<double>(at) / sr;
    const std::vector<ks::TimedEvent> events = {{t, ks::MidiEvent::noteOn(60, 100)},
                                                {t + 0.25, ks::MidiEvent::noteOff(60)}};
    const ks::RenderResult r = ks::OfflineRenderer::render(patch, events, opt);
    float floor = 0.0f;
    for (int64_t i = 0; i < at && i < static_cast<int64_t>(r.left.size()); ++i)
        floor = std::max({floor, std::fabs(r.left[static_cast<size_t>(i)]), std::fabs(r.right[static_cast<size_t>(i)])});
    const float thr = std::max(1.0e-6f, 4.0f * floor);
    for (int64_t i = at; i < static_cast<int64_t>(r.left.size()); ++i)
        if (std::fabs(r.left[static_cast<size_t>(i)]) > thr || std::fabs(r.right[static_cast<size_t>(i)]) > thr)
            return i - at;
    return -1;
}

int runLatency(const ks::AppPaths& paths, double sr, int block) {
    const ks::PresetStore store(paths);
    const int offsets[2] = {0, std::min(37, block - 1)};
    std::printf("ks-bench --latency: note-on -> first sample > noise floor, %.0f Hz, block %d\n", sr, block);
    std::printf("%-22s %14s %14s  %s\n", "engine", "@0 smp (ms)", "@37 smp (ms)", "source");
    int worst = 0;
    for (const ks::ModuleInfo* info : ks::defaultRegistry().list()) {
        if (info->kind != ks::ModuleKind::Instrument) continue;
        std::optional<ks::Patch> patch;
        std::string source = "default params";
        if (info->typeId == "sampler") {
            source = "skipped: no factory sampler preset with an installed library";
            for (const auto& p : store.list()) {
                if (!p.factory) continue;
                ks::Patch pp = store.load(p.path);
                if (pp.layers.empty() || pp.layers[0].instrument.type != "sampler") continue;
                const std::string sfz = pp.layers[0].instrument.state.value("sfz", std::string());
                if (!ks::resolveSamplePath(sfz, paths.root).path) continue;
                pp.layers.resize(1);
                patch = pp;
                source = p.path;
                break;
            }
        } else {
            ks::Patch p;
            ks::Layer l;
            l.instrument.type = info->typeId;
            p.layers.push_back(l);
            patch = p;
        }
        if (!patch) {
            std::printf("%-22s %14s %14s  %s\n", info->typeId.c_str(), "-", "-", source.c_str());
            continue;
        }
        char cols[2][32];
        for (int k = 0; k < 2; ++k) {
            const int64_t lat = noteLatency(*patch, offsets[k], sr, block);
            if (lat < 0) std::snprintf(cols[k], sizeof(cols[k]), "silent");
            else std::snprintf(cols[k], sizeof(cols[k]), "%lld (%.2f)", static_cast<long long>(lat), 1000.0 * lat / sr);
            worst = std::max(worst, lat < 0 ? block + 1 : static_cast<int>(lat));
        }
        std::printf("%-22s %14s %14s  %s\n", info->typeId.c_str(), cols[0], cols[1], source.c_str());
    }
    std::printf("worst: %d samples (%s)\n", worst, worst <= block ? "OK, within one block" : "FAIL, > one block");
    return worst <= block ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    double sr = 48000.0, seconds = 10.0;
    int block = 64;
    std::string filter;
    bool latency = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string("0"); };
        if (a == "--sr") sr = std::stod(next());
        else if (a == "--block") block = std::stoi(next());
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--filter") filter = next();
        else if (a == "--latency") latency = true;
        else {
            std::fprintf(stderr, "usage: ks-bench [--sr HZ] [--block N] [--seconds S] [--filter SUBSTR] [--latency]\n");
            return 2;
        }
    }
    const ks::AppPaths paths = ks::AppPaths::discover();
    if (latency) return runLatency(paths, sr, std::max(1, block));
    const ks::PresetStore store(paths);
    const auto base = ks::OfflineRenderer::standardTestEvents();
    double patternLen = 0.0;
    for (const auto& e : base) patternLen = std::max(patternLen, e.time);
    patternLen += 0.5;
    std::vector<ks::TimedEvent> events;
    for (double t0 = 0.0; t0 + patternLen <= seconds || events.empty(); t0 += patternLen)
        for (auto e : base) {
            e.time += t0;
            events.push_back(e);
        }

    std::printf("ks-bench: %.0f Hz, block %d, %.1f s per preset\n", sr, block, seconds);
    std::printf("%-48s %10s %10s\n", "preset", "rt-factor", "ms/block");
    double worst = 1e30;
    int count = 0;
    for (const auto& p : store.list()) {
        if (!p.factory || (!filter.empty() && p.path.find(filter) == std::string::npos)) continue;
        ks::Patch patch;
        try {
            patch = store.load(p.path);
        } catch (const std::exception& e) {
            std::printf("%-48s  load error: %s\n", p.path.c_str(), e.what());
            continue;
        }
        ks::RenderOptions opt;
        opt.sampleRate = sr;
        opt.blockSize = block;
        opt.tailSeconds = 0.5;
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = ks::OfflineRenderer::render(patch, events, opt);
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rtf = r.seconds() / std::max(wall, 1e-9);
        const double blocks = static_cast<double>(r.left.size()) / block;
        std::printf("%-48s %9.1fx %10.4f\n", p.path.c_str(), rtf, 1000.0 * wall / std::max(blocks, 1.0));
        worst = std::min(worst, rtf);
        ++count;
    }
    if (count == 0) {
        std::printf("no factory presets found under %s\n", ks::pathToUtf8(paths.factoryPresets).c_str());
        return 1;
    }
    std::printf("worst: %.1fx realtime (%s)\n", worst, worst >= 2.0 ? "OK, >= 2x headroom" : "FAIL, < 2x headroom");
    return worst >= 2.0 ? 0 : 1;
}
