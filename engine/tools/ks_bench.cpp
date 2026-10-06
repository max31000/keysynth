// ks-bench: realtime factor per factory preset (ARCHITECTURE §12: worst preset needs >= 2x at 48 kHz / 64).
//   ks-bench [--sr 48000] [--block 64] [--seconds 10] [--filter substr]
// Renders the standard test pattern repeated to fill --seconds, measures wall time.
// realtime factor = audio seconds / wall seconds.

#include "core/AppPaths.h"
#include "core/ModuleRegistry.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    double sr = 48000.0, seconds = 10.0;
    int block = 64;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string("0"); };
        if (a == "--sr") sr = std::stod(next());
        else if (a == "--block") block = std::stoi(next());
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--filter") filter = next();
        else {
            std::fprintf(stderr, "usage: ks-bench [--sr HZ] [--block N] [--seconds S] [--filter SUBSTR]\n");
            return 2;
        }
    }
    const ks::AppPaths paths = ks::AppPaths::discover();
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
