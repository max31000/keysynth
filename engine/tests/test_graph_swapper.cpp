#include "core/Engine.h"
#include "core/GraphBuilder.h"
#include "core/ModuleRegistry.h"
#include "core/PatchModel.h"
#include "core/RtCheck.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <random>
#include <thread>

using namespace ks;

namespace {
void publish(Engine& e, const PatchModel& m, bool reuse = true) {
    auto r = GraphBuilder::build(m.patch(), defaultRegistry(), reuse ? e.latestGraph() : nullptr, e.sampleRate(), e.maxBlock());
    e.publish(std::move(r.graph));
}

float renderBlocks(Engine& e, int blocks, std::vector<MidiEvent> first = {}) {
    std::vector<float> l(static_cast<size_t>(e.maxBlock())), r(static_cast<size_t>(e.maxBlock()));
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        AudioBlock ab{l.data(), r.data(), e.maxBlock()};
        e.processBlock(ab, b == 0 ? MidiEventSpan(first.data(), first.size()) : MidiEventSpan());
        for (int i = 0; i < ab.numSamples; ++i) peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    }
    return peak;
}
} // namespace

TEST_CASE("GraphSwapper: first publish goes live, shared modules keep sounding", "[swap]") {
    const int base = Module::liveInstances();
    {
        Engine e;
        e.prepare(48000.0, 64);
        PatchModel m(defaultRegistry());
        publish(e, m);
        REQUIRE(renderBlocks(e, 4, {MidiEvent::noteOn(60, 100)}) > 0.01f);
        REQUIRE(e.swapper().current() != nullptr);
        const Module* instr = e.swapper().current()->layers[0]->instrument.module.get();

        // Structural edit that keeps the instrument: shared -> crossfade, note keeps sounding.
        m.addFx(m.patch().layers[0].node, "gain", std::nullopt);
        publish(e, m);
        renderBlocks(e, 1);
        REQUIRE(e.swapper().inTransition());
        REQUIRE(e.swapper().current()->layers[0]->instrument.module.get() == instr);
        const float during = renderBlocks(e, 30); // > 20 ms
        REQUIRE(during > 0.01f);
        REQUIRE_FALSE(e.swapper().inTransition());
        REQUIRE(instr->activeVoices() == 1);
        e.collectGarbage();
        REQUIRE(e.swapper().ownedCount() == 1);

        // Preset-like switch without reuse: old graph tails out, then is retired.
        publish(e, m, false);
        renderBlocks(e, 1);
        REQUIRE(e.swapper().inTransition());
        renderBlocks(e, 48000 / 64 + 10);
        REQUIRE_FALSE(e.swapper().inTransition());
        e.collectGarbage();
        REQUIRE(e.swapper().ownedCount() == 1);
    }
    REQUIRE(Module::liveInstances() == base);
}

TEST_CASE("GraphSwapper stress: 10k random publishes while rendering", "[swap][stress]") {
    rt::resetViolations();
    const int base = Module::liveInstances();
    {
        Engine e;
        e.prepare(48000.0, 64);
        PatchModel m(defaultRegistry());
        publish(e, m);

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> blocks{0};
        std::atomic<bool> bad{false};
        std::thread audio([&] {
            std::vector<float> l(64), r(64);
            std::mt19937 rng(1);
            std::vector<MidiEvent> ev;
            ev.reserve(8);
            while (!stop.load()) {
                ev.clear();
                const uint32_t x = rng();
                if (x % 7 == 0) ev.push_back(MidiEvent::noteOn(36 + static_cast<int>(x % 60), 100, 1, x % 64));
                if (x % 5 == 0) ev.push_back(MidiEvent::noteOff(36 + static_cast<int>((x >> 8) % 60), 0, 1, 63));
                if (x % 97 == 0) ev.push_back(MidiEvent::cc(64, (x >> 4) % 2 ? 127 : 0, 1, 63));
                AudioBlock ab{l.data(), r.data(), 64};
                e.processBlock(ab, MidiEventSpan(ev.data(), ev.size()));
                for (int i = 0; i < 64; ++i)
                    if (!std::isfinite(l[i]) || !std::isfinite(r[i]) || std::fabs(l[i]) > 1.0f) bad.store(true);
                blocks.fetch_add(1);
            }
        });

        std::mt19937 rng(42);
        auto pick = [&](size_t n) { return static_cast<size_t>(rng() % n); };
        for (int i = 0; i < 10000; ++i) {
            const auto& p = m.patch();
            switch (rng() % 9) {
            case 0:
                if (p.layers.size() < 6) m.addLayer(std::nullopt, "basic");
                break;
            case 1:
                if (p.layers.size() > 1) m.removeLayer(p.layers[pick(p.layers.size())].node);
                break;
            case 2:
                if (!p.layers.empty()) m.addFx(p.layers[pick(p.layers.size())].node, rng() % 2 ? "gain" : "limiter", std::nullopt);
                break;
            case 3:
                for (const auto& l : p.layers)
                    if (!l.fx.empty()) {
                        m.removeFx(l.fx[0].node);
                        break;
                    }
                break;
            case 4:
                if (!p.layers.empty()) m.setInstrument(p.layers[pick(p.layers.size())].node, "basic");
                break;
            case 5:
                if (p.master.fx.size() < 3) m.addFx(kMasterNode, "gain", std::nullopt);
                else m.removeFx(p.master.fx[0].node);
                break;
            case 6:
                m.setPatch(PatchModel::makeDefaultPatch());
                break;
            default:
                if (!p.layers.empty()) {
                    const NodeId n = p.layers[pick(p.layers.size())].instrument.node;
                    const float v = m.setParam(n, "cutoff", static_cast<float>(100 + rng() % 10000));
                    if (Module* mod = e.latestGraph() ? e.latestGraph()->findModule(n) : nullptr) mod->params().set("cutoff", v);
                }
                break;
            }
            publish(e, m, rng() % 10 != 0);
            if (i % 16 == 0) e.collectGarbage();
            if (i % 64 == 0) std::this_thread::yield();
        }
        // Let the audio thread finish transitions.
        const uint64_t target = blocks.load() + 2000;
        while (blocks.load() < target) std::this_thread::yield();
        stop.store(true);
        audio.join();
        e.collectGarbage();
        REQUIRE_FALSE(bad.load());
        REQUIRE(e.swapper().ownedCount() <= 2);
        REQUIRE(e.swapper().transitionsStarted() > 10);
    }
    REQUIRE(Module::liveInstances() == base); // no leaked modules
    if (rt::checksEnabled()) REQUIRE(rt::violationCount() == 0);
}

TEST_CASE("RtCheck detects allocation inside RtScope", "[rt]") {
    if (!rt::checksEnabled()) SKIP("KS_RT_CHECKS off");
    rt::resetViolations();
    {
        rt::RtScope s;
        int* volatile p = new int(3);
        delete p;
    }
    REQUIRE(rt::violationCount() == 2);
    rt::resetViolations();
    {
        ks::Mutex mu;
        rt::RtScope s;
        mu.lock();
        mu.unlock();
    }
    REQUIRE(rt::violationCount() == 1);
    rt::resetViolations();
}
