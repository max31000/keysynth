# msfa (vendored)

DX7 FM core ("music synthesizer for android", Google, Apache-2.0) as maintained in Dexed.

- Source: https://github.com/asb2m10/dexed, commit `2e182b3db85c09083ab13c8b9b00565ce7d9ff85`, directory `Source/msfa/`.
- Licence: Apache License 2.0 (`LICENSE`); copyright headers kept in every file.
- Built as the isolated static library `ks_msfa` (warnings off), used only by `engine/src/instruments/fm/`.

`dexed/` holds Dexed's alternative operator engines `EngineMkI` and `EngineOpl` from `Source/` of the same
commit. They are **GPL-3.0-or-later** (Pascal Gauthier, `dexed/COPYING`), compatible with keysynth's
AGPL-3.0-or-later (GPLv3 §13 / AGPLv3 §13). Modification: `EngineMkI.cpp` constructor fills its static tables only once (marked `keysynth:`).

## Local modifications (Apache-2.0 §4(b))

- `controllers.h`, `env.cc`: removed `#include "../Dexed.h"` (Dexed plugin header, unused here).
- `tuning.h`, `tuning.cc`: replaced by a 12-TET-only implementation (Dexed's needs JUCE + Surge Tunings).
- `libMTSClient.h`: new stub (no MTS-ESP master ever present).
- `dx7note.cc`: the constructor zeroes the feedback buffer (was uninitialized); `update()` also resets the
  portamento pitch (no glide chirp on legato / live edits).
- `env.cc`, `env.h`, `dx7note.h`: a re-initialized envelope restarts from its current level instead of 0 (no click
  on re-strike / voice steal, as on the DX7); new `Env::forget()` / `Dx7Note::forget()` for a hard reset.
- `synth.h`: block size `N` = 8 (`LG_N` = 3, upstream 64) so events take effect within 8 samples. All rates are
  scaled by `N`/`LG_N` (env `inc_`, lfo `unit_`/`lforatio_`, pitchenv `unit_`, porta rates, gain interpolation,
  phase advance), so timings are unchanged. Precision fixes for the smaller per-block increments: `env.cc` rounds
  the sample-rate scaling of `inc_` (was truncated), `pitchenv.cc` keeps `unit_` in Q8.
- Not vendored: Dexed's JUCE submodule, MTS-ESP, the tuning library, any GUI/plugin code.
