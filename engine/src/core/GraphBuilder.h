#pragma once
// GraphBuilder (control thread): Patch -> new RackGraph, diffing against the previous graph so unchanged
// modules ({nodeId, type, state} equal, same sampleRate/maxBlock) are *shared* (shared_ptr), not rebuilt
// (ARCHITECTURE §5.4). New modules are constructed, given params, loadState()d and prepare()d here.

#include "core/ModuleRegistry.h"
#include "core/Patch.h"
#include "core/RackGraph.h"

#include <memory>
#include <string>
#include <vector>

namespace ks {

struct BuildResult {
    std::unique_ptr<RackGraph> graph;
    std::vector<std::string> warnings;
    // Modules whose loadState() wrote params (Module::loadStateWroteParams): their full params and new state, to be
    // copied back into the patch (PatchModel::adoptModuleState) so the loaded values become the patch's.
    struct Adopted {
        NodeId node = 0;
        std::map<std::string, float> params;
        nlohmann::json state;
    };
    std::vector<Adopted> adopted;
    int reused = 0;
    int created = 0;
};

class GraphBuilder {
public:
    // `previous` may be null (no reuse). Params are read from `patch` (PatchModel) at build time.
    static BuildResult build(const Patch& patch, const ModuleRegistry& registry, const RackGraph* previous,
                             double sampleRate, int maxBlock);
};

} // namespace ks
