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
