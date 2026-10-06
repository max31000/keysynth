// Built-in module registration: one line per module (ARCHITECTURE §3, §13).
#include "core/ModuleRegistry.h"

#include "effects/gain/GainFx.h"
#include "effects/limiter/LimiterFx.h"
#include "effects/tremolo/TremoloFx.h"
#include "instruments/basic/BasicSynth.h"
#include "instruments/epiano/EPiano.h"

namespace ks {

void registerBuiltinModules(ModuleRegistry& r) {
    // Instruments
    r.add<BasicSynth>();
    r.add<EPiano>();
    // Effects
    r.add<GainFx>();
    r.add<LimiterFx>();
    r.add<TremoloFx>();
}

} // namespace ks
