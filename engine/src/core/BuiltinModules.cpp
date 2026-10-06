// Built-in module registration: one line per module (ARCHITECTURE §3, §13).
#include "core/ModuleRegistry.h"

#include "effects/gain/GainFx.h"
#include "effects/limiter/LimiterFx.h"
#include "effects/rotary/RotaryFx.h"
#include "instruments/basic/BasicSynth.h"
#include "instruments/combo/ComboOrgan.h"
#include "instruments/organ/ToneWheelOrgan.h"
#include "instruments/va/VaSynth.h"

namespace ks {

void registerBuiltinModules(ModuleRegistry& r) {
    // Instruments
    r.add<BasicSynth>();
    r.add<ToneWheelOrgan>();
    r.add<ComboOrgan>();
    r.add<VaSynth>();
    // Effects
    r.add<GainFx>();
    r.add<LimiterFx>();
    r.add<RotaryFx>();
}

} // namespace ks
