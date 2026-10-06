// Built-in module registration: one line per module (ARCHITECTURE §3, §13).
#include "core/ModuleRegistry.h"

#include "effects/chorus/ChorusFx.h"
#include "effects/compressor/CompressorFx.h"
#include "effects/delay/DelayFx.h"
#include "effects/drive/DriveFx.h"
#include "effects/ensemble/EnsembleFx.h"
#include "effects/eq/EqFx.h"
#include "effects/flanger/FlangerFx.h"
#include "effects/gain/GainFx.h"
#include "effects/limiter/LimiterFx.h"
#include "effects/rotary/RotaryFx.h"
#include "effects/phaser/PhaserFx.h"
#include "effects/reverb/ReverbFx.h"
#include "instruments/basic/BasicSynth.h"
#include "instruments/combo/ComboOrgan.h"
#include "instruments/organ/ToneWheelOrgan.h"
#include "instruments/va/VaSynth.h"
#include "instruments/fm/FmSynth.h"
#include "effects/tremolo/TremoloFx.h"
#include "instruments/epiano/EPiano.h"

namespace ks {

void registerBuiltinModules(ModuleRegistry& r) {
    // Instruments
    r.add<BasicSynth>();
    r.add<ToneWheelOrgan>();
    r.add<ComboOrgan>();
    r.add<VaSynth>();
    r.add<FmSynth>();
    // Effects
    r.add<GainFx>();
    r.add<LimiterFx>();
    r.add<RotaryFx>();
    r.add<EPiano>();
    r.add<TremoloFx>();
    r.add<ChorusFx>();
    r.add<EnsembleFx>();
    r.add<PhaserFx>();
    r.add<FlangerFx>();
    r.add<DelayFx>();
    r.add<ReverbFx>();
    r.add<DriveFx>();
    r.add<CompressorFx>();
    r.add<EqFx>();
}

} // namespace ks
