# Third-party notices

## Sample libraries

The sample libraries are **not** stored in this repository. `scripts/fetch_samples.py` downloads them into
`assets/samples/` (git-ignored) from the URLs in `assets/samples.json`. Each library keeps its own licence file inside
its directory. Libraries marked non-commercial or unclear must not be bundled in a commercial product.

| Library | Author / source | Licence | Redistribution |
|---|---|---|---|
| Salamander Grand Piano V3 (SFZ+FLAC) | Alexander Holm; packaging by FreePats (freepats.zenvoid.org) | CC BY 3.0 | Allowed with attribution |
| jRhodes3c | Jeff Learman (github.com/sfzinstruments/jlearman.jRhodes3c) | Samples CC BY-NC 4.0, SFZ files CC0 | Non-commercial only |
| E-Pianos (Wurlitzer EP200, Yamaha CP80, Hohner Pianet T) | Greg Sullivan (github.com/sfzinstruments/GregSullivan.E-Pianos) | CC BY 3.0 | Allowed with attribution |
| MelloSFZotron | Mellotron M400 samples by Bernie Kornowicz (taijiguy / Leisureland); SFZ by Nathan Ingelbrecht; musical-artifacts.com #5947 | Samples: free to use, not in commercial or for-profit software. SFZ files CC0 | Non-commercial only |
| Sonatina Symphonic Orchestra 1.0 | Mattias Westlund; musical-artifacts.com #81 | Creative Commons Sampling Plus 1.0 | Non-commercial redistribution of the samples |
| Virtual Playing Orchestra 3.2 waves / 3.3 scripts | Paul Battersby (virtualplaying.com); built on Sonatina Symphonic Orchestra (CC Sampling+), VSCO 2 CE (CC0), No Budget Orchestra, University of Iowa MIS, Philharmonia Orchestra samples | Mixed CC per source; no restrictions on music use | Per component licence |
| Drawbar / Percussive / Rock Organ Emulation | FreePats (freepats.zenvoid.org) | CC0 1.0 | Allowed |
| Clavecin | SFZ conversion by S. Christian Collins (github.com/sfzinstruments/Clavecin) | No explicit licence | Personal use; do not bundle |
| Orgue Eglise | SFZ conversion by S. Christian Collins (github.com/sfzinstruments/OrgueEglise) | No explicit licence | Personal use; do not bundle |
| Clavinet | Lithalean; musical-artifacts.com #646 | Unclear provenance | Personal use; do not bundle |
| Big Rusty Drums 1.100 | Karoryfer Samples (github.com/sfzinstruments/karoryfer.big-rusty-drums) | CC0 1.0 | Allowed |
| Linnoleum SFZ-1 v2 (LinnDrum) | musical-artifacts.com #6356 | SFZ free; LinnDrum ROM samples of unclear status | Personal use; do not bundle |

Attribution text for CC BY libraries:
- "Salamander Grand Piano" by Alexander Holm, licensed under CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/).
- "E-Pianos" by Greg Sullivan, licensed under CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/).
- "jRhodes3c" by Jeff Learman, samples licensed under CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/).
- "Sonatina Symphonic Orchestra" by Mattias Westlund, licensed under CC Sampling Plus 1.0
  (https://creativecommons.org/licenses/sampling+/1.0/).

Local fixes applied after download (see `patches` in `assets/samples.json`): Linnoleum `d22` hi-hat sample paths and
SSO `Concert Harp.sfz` root-absolute paths. These change only the SFZ mapping text, not the samples.

## Code

| Component | Source | Licence | Where |
|---|---|---|---|
| MSFA (music-synthesizer-for-android DX7 core), Dexed fork | Google Inc., Pascal Gauthier, Jean Pierre Cimalando; github.com/asb2m10/dexed `Source/msfa` @ 2e182b3 | Apache-2.0 (modified, see `third_party/msfa/README.md`) | `third_party/msfa/` |
| Dexed EngineMkI / EngineOpl | Pascal Gauthier; EngineOpl based on ppplay (Steffen Ohrendorf) and OPL3 Java code (Robson Cozendey); github.com/asb2m10/dexed `Source/` @ 2e182b3 | GPL-3.0-or-later (compatible with this AGPL-3.0-or-later project); `EngineMkI.cpp` modified | `third_party/msfa/dexed/` |

DX7 banks: `assets/dx7/keysynth-fm-factory.syx` contains only original keysynth voices. Third-party DX7 cartridges
(Yamaha ROM banks, Dexed's bundled carts) are not redistributed.
## sfizz (sampler engine)
The `sampler` module links **sfizz 1.2.3** (github.com/sfztools/sfizz), BSD-2-Clause, Copyright (c) sfizz
contributors. It is fetched by CMake (`cmake/Dependencies.cmake`, which applies one local one-line fix to
`src/sfizz/ADSREnvelope.cpp`, documented there). Bundled sfizz components compiled in, with their licences (texts in
the sfizz source tree under `external/` and `src/external/`): Abseil (Apache-2.0), SIMDe (MIT), ghc::filesystem (MIT),
atomic_queue (MIT), jsl (BSL-1.0), cephes (BSD-3-Clause style), st_audiofile (BSD-2-Clause), dr_libs and stb_vorbis
(public domain / MIT-0), libaiff (MIT-style), WavPack (BSD-3-Clause), KISS FFT (BSD-3-Clause), pugixml (MIT),
cpuid (BSD-3-Clause), spline (BSD-3-Clause), tunings (MIT), hiir (WTFPL).

## Faust (plugin JIT)

Faust 2.88 (GRAME, https://faust.grame.fr) is **not** stored in this repository: `scripts/fetch_faust.ps1` unpacks it
into `.tools/faust/` (git-ignored). The engine loads `faust.dll` (libfaust with LLVM 17 linked in) at runtime and
includes its C headers at build time. libfaust: LGPL-2.1-or-later; LLVM: Apache-2.0 with LLVM exception; the
architecture headers carry the GRAME architecture exception. Faust standard libraries (`share/faust/*.lib`) used by
the example plugins are under their per-file licences (mostly LGPL / MIT-style; see each `.lib` header). Code
generated by the Faust compiler from our `.dsp` files is ours (AGPL-3.0-or-later).
