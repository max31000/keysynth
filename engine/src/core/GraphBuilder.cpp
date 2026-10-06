#include "core/GraphBuilder.h"

namespace ks {

namespace {

struct Ctx {
    const ModuleRegistry& registry;
    const RackGraph* previous;
    double sampleRate;
    int maxBlock;
    BuildResult& result;
};

void fillNode(Ctx& c, ModuleNode& dst, const ModuleSlot& slot, ModuleKind expected) {
    dst.node = slot.node;
    dst.type = slot.type;
    dst.state = slot.state;
    dst.bypass.store(slot.bypass);

    const ModuleInfo* info = c.registry.find(slot.type);
    if (info == nullptr || info->kind != expected) {
        if (!slot.type.empty())
            c.result.warnings.push_back("module '" + slot.type + "' unavailable for node " + std::to_string(slot.node));
        return;
    }

    if (c.previous && c.previous->sampleRate() == c.sampleRate && c.previous->maxBlock() == c.maxBlock) {
        if (const ModuleNode* old = c.previous->findModuleNode(slot.node)) {
            if (old->module && old->type == slot.type && old->state == slot.state) {
                dst.module = old->module;
                for (const auto& [id, v] : slot.params) dst.module->params().set(id, v);
                ++c.result.reused;
                return;
            }
        }
    }

    std::unique_ptr<Module> m = c.registry.create(slot.type);
    if (!m) {
        c.result.warnings.push_back("factory for '" + slot.type + "' failed");
        return;
    }
    for (const auto& [id, v] : slot.params) m->params().set(id, v);
    m->loadState(slot.state);
    m->prepare(c.sampleRate, c.maxBlock);
    dst.module = std::shared_ptr<Module>(std::move(m));
    ++c.result.created;
}

} // namespace

BuildResult GraphBuilder::build(const Patch& patch, const ModuleRegistry& registry, const RackGraph* previous,
                                double sampleRate, int maxBlock) {
    BuildResult r;
    r.graph = std::make_unique<RackGraph>(sampleRate, maxBlock);
    Ctx c{registry, previous, sampleRate, maxBlock, r};
    RackGraph& g = *r.graph;

    for (const Layer& l : patch.layers) {
        auto ln = std::make_unique<LayerNode>();
        ln->node = l.node;
        ln->setZone(l.zone);
        fillNode(c, ln->instrument, l.instrument, ModuleKind::Instrument);
        for (const ModuleSlot& f : l.fx) {
            auto fn = std::make_unique<ModuleNode>();
            fillNode(c, *fn, f, ModuleKind::Effect);
            ln->fx.push_back(std::move(fn));
        }
        // Carry sounding-note mapping over so note-offs for held keys reach a shared instrument.
        if (previous) {
            if (const LayerNode* old = previous->findLayer(l.node)) {
                for (size_t i = 0; i < ln->noteMap.size(); ++i)
                    ln->noteMap[i].store(old->noteMap[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
        }
        g.layers.push_back(std::move(ln));
    }
    for (const ModuleSlot& f : patch.master.fx) {
        auto fn = std::make_unique<ModuleNode>();
        fillNode(c, *fn, f, ModuleKind::Effect);
        g.masterFx.push_back(std::move(fn));
    }
    fillNode(c, g.rhythm.drums, patch.rhythm.drums, ModuleKind::Instrument);
    g.masterVolumeDb.store(patch.master.volumeDb);
    g.finalize();
    return r;
}

} // namespace ks
