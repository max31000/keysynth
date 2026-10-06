// Note-on -> first audible output sample, through the whole graph (OfflineRenderer: zone filter, layer fx,
// master section + limiter). `fm` must be accurate to <= 8 samples (MSFA block N = 8); every instrument engine
// must start within one device block of the note-on. The same measurement is `ks-bench --latency`.

#include "core/AppPaths.h"
#include "core/ModuleRegistry.h"
#include "instruments/sampler/SamplePaths.h"
#include "preset/PresetStore.h"
#include "render/OfflineRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace ks;

namespace {

constexpr int kPreBlocks = 4; // blocks rendered before the note-on (noise floor reference)

// Samples from the note-on to the first output sample above the noise floor (max |x| before the note, but at
// least 1e-6 = -120 dBFS), or -1 if the note never gets there.
int64_t noteLatency(const Patch& patch, int offset, double sr, int block, int note = 60) {
    const int64_t at = static_cast<int64_t>(kPreBlocks) * block + offset;
    RenderOptions opt;
    opt.sampleRate = sr;
    opt.blockSize = block;
    opt.tailSeconds = 0.05;
    const double t = static_cast<double>(at) / sr;
    const std::vector<TimedEvent> events = {{t, MidiEvent::noteOn(note, 100)},
                                            {t + 0.25, MidiEvent::noteOff(note)}};
    const RenderResult r = OfflineRenderer::render(patch, events, opt);
    float floor = 0.0f;
    for (int64_t i = 0; i < at && i < static_cast<int64_t>(r.left.size()); ++i)
        floor = std::max({floor, std::fabs(r.left[static_cast<size_t>(i)]), std::fabs(r.right[static_cast<size_t>(i)])});
    const float thr = std::max(1.0e-6f, 4.0f * floor);
    for (int64_t i = at; i < static_cast<int64_t>(r.left.size()); ++i)
        if (std::fabs(r.left[static_cast<size_t>(i)]) > thr || std::fabs(r.right[static_cast<size_t>(i)]) > thr)
            return i - at;
    return -1;
}

// One layer with the module's default params (sampler: first factory preset whose library is installed).
std::optional<Patch> enginePatch(const std::string& typeId, std::string& note) {
    if (typeId != "sampler") {
        Patch p;
        Layer l;
        l.name = typeId;
        l.instrument.type = typeId;
        p.layers.push_back(l);
        return p;
    }
    const AppPaths paths = AppPaths::fromRoot(KS_SOURCE_DIR);
    const PresetStore store(paths);
    for (const auto& info : store.list()) {
        if (!info.factory) continue;
        const Patch p = store.load(info.path);
        if (p.layers.empty() || p.layers[0].instrument.type != "sampler") continue;
        const std::string sfz = p.layers[0].instrument.state.value("sfz", std::string());
        if (!resolveSamplePath(sfz, paths.root).path) continue;
        Patch one = p;
        one.layers.resize(1);
        note = info.path;
        return one;
    }
    note = "no factory sampler preset with an installed library";
    return std::nullopt;
}

} // namespace

TEST_CASE("note latency: fm note-on is sample accurate (<= 8 samples)", "[latency][fm]") {
    Patch patch;
    Layer l;
    l.instrument.type = "fm";
    patch.layers.push_back(l);
    for (const int block : {32, 64, 100, 256}) {
        for (const int offset : {0, 1, 7, 13, 31, 40, 63, 99, 200}) {
            if (offset >= block) continue;
            INFO("block " << block << " offset " << offset);
            const int64_t lat = noteLatency(patch, offset, 48000.0, block);
            CHECK(lat >= 0);
            CHECK(lat <= 8);
        }
    }
}

namespace {
// `sampler` loads a real sample library (seconds): only in the hidden [samples] tier.
int checkEngines(bool samplerOnly) {
    constexpr int kBlock = 64;
    int measured = 0;
    for (const ModuleInfo* info : defaultRegistry().list()) {
        if (info->kind != ModuleKind::Instrument || (info->typeId == "sampler") != samplerOnly) continue;
        std::string note;
        const std::optional<Patch> patch = enginePatch(info->typeId, note);
        if (!patch) {
            WARN(info->typeId << ": skipped (" << note << ")");
            continue;
        }
        for (const int offset : {0, 37}) {
            const int64_t lat = noteLatency(*patch, offset, 48000.0, kBlock);
            INFO(info->typeId << (note.empty() ? "" : " (" + note + ")") << " offset " << offset << ": latency "
                               << lat << " samples");
            CHECK(lat >= 0);
            CHECK(lat <= kBlock);
        }
        ++measured;
    }
    return measured;
}
} // namespace

TEST_CASE("note latency: every instrument engine starts within one block", "[latency]") {
    CHECK(checkEngines(false) >= 7); // basic va fm organ combo epiano drums (+ plugin instruments if registered)
}

TEST_CASE("note latency: sampler starts within one block (installed libraries only)", "[.samples][latency]") {
    checkEngines(true);
}
