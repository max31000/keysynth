# Free SFZ Sample Libraries + Faust for Windows (research, 2026-10-06)

Legend: **V** = URL checked on 2026-10-06 with HEAD/GET, plain HTTP and no login (status 200; size from Content-Length).
GitHub `archive/refs/...zip` URLs return 200 but are streamed with no Content-Length, so their sizes come from summing the git tree blobs through the GitHub API.
Sizes are in decimal MB/GB unless marked MiB/GiB.

> sfizz cannot read SF2 or GIG. An SF2 needs converting first (e.g. Polyphone "export to SFZ"). Everything below is native SFZ unless noted.

---

## 1. Acoustic grand piano

| Name | Licence | Format | Download | Unpacked | URL |
|---|---|---|---|---|---|
| **Salamander Grand Piano V3+20200602** (Yamaha C5, 16 vel layers, 48k/24, release + hammer noise) | CC-BY 3.0 (Alexander Holm) | SFZ + FLAC | **741.8 MB** (tar.gz) **V** | ~707 MiB (freepats page) | https://freepats.zenvoid.org/Piano/SalamanderGrandPiano/SalamanderGrandPiano-SFZ+FLAC-V3+20200602.tar.gz |
| Salamander V3 SFZ+WAV 48k/24 | CC-BY 3.0 | SFZ + WAV | 1.258 GB (tar.xz) **V** | 1.18 GiB+ | https://freepats.zenvoid.org/Piano/SalamanderGrandPiano/SalamanderGrandPianoV3+20161209_48khz24bit.tar.xz |
| Salamander V3 44.1k/16 (lighter) | CC-BY 3.0 | SFZ + WAV | 412 MB (tar.xz) **V** | ~0.6 GB | https://freepats.zenvoid.org/Piano/SalamanderGrandPiano/SalamanderGrandPianoV3+20161209_44khz16bit.tar.xz |
| Salamander mirror (archive.org, tar.bz2 48k/24) | CC-BY 3.0 | SFZ + WAV | 1.448 GB **V** | — | https://archive.org/download/SalamanderGrandPianoV3/SalamanderGrandPianoV3_48khz24bit.tar.bz2 |
| Salamander (sfzinstruments repo, FLAC, improved mapping) | CC-BY 3.0 | SFZ + FLAC | git archive | 713 MB | https://github.com/sfzinstruments/SalamanderGrandPiano/archive/refs/heads/master.zip |
| Splendid Grand Piano (Akai Steinway, old public-domain set) | Public domain | SFZ + FLAC | git archive | 73 MB | https://github.com/sfzinstruments/SplendidGrandPiano/archive/refs/heads/master.zip |
| Upright Piano KW (light fallback) | CC0 | SFZ + FLAC | 33 MB **V** | ~40 MB | https://freepats.zenvoid.org/Piano/UprightPianoKW/UprightPianoKW-SFZ+FLAC-20220221.7z |

Notes: Salamander SFZ+FLAC is the best fit. It is lossless, half the size of the WAV version, and sfizz decodes FLAC natively. Splendid is a small, low-RAM fallback.
The freepats `SalamanderGrandPiano-SFZ+FLAC-V3+20200602.tar.xz` filename does not exist (404). Only the `.tar.gz` is published.

## 2. Electric pianos (Rhodes / Wurlitzer / CP80 / Pianet)

| Name | Licence | Format | Size (unpacked) | URL |
|---|---|---|---|---|
| **jRhodes3c** (1977 Rhodes Mk I Stage 73, 5 vel layers, looped, stereo option, 3 .sfz files) | Playing and music use: free. **Redistributing the samples: CC BY-NC 4.0** (the licence text says "BY-NC-SA" but links to BY-NC; commercial use needs permission from the author). Control files are CC0. | SFZ + FLAC | 18 MB | https://github.com/sfzinstruments/jlearman.jRhodes3c/archive/refs/heads/master.zip |
| jRhodes3d (full-length, unlooped samples, 13 .sfz files) | same | SFZ + FLAC | 92 MB | https://github.com/sfzinstruments/jlearman.jRhodes3d/archive/refs/heads/master.zip |
| **Greg Sullivan E-Pianos**: Wurlitzer EP200, Yamaha CP80, Hohner Pianet T | CC-BY 3.0 | SFZ + FLAC | 20 MB | https://github.com/sfzinstruments/GregSullivan.E-Pianos/archive/refs/heads/master.zip |
| Wurlitzer (SFZ, Lithalean) | "gray" (unclear provenance) | SFZ + WAV | 44 MB zip **V** | https://musical-artifacts.com/artifacts/645/Wurlitzer.zip |

## 3. Mellotron (M400: flute, strings, choir)

All of these use the **taijiguy / Leisureland** samples: a 1973 M400S, serial #500. Sounds include MkII Flute, MkII 3 Violins (the "M400 violins"), String Section, Cello, Combined Choir, 8 Choir, Woodwind 2, GC3 Brass, M300A/B and more. Each sample is about 7 s and **unlooped**, like a real tape. Licence: free to use, **but not in commercial or for-profit software** (statement from the owner, Bernie Kornowicz). Fine for a personal app.

| Name | Format | Size | URL |
|---|---|---|---|
| **MelloSFZotron** (Nathan Ingelbrecht). Key-switch and CC2 sound select, tone LPF (CC74), attack/release, aftertouch "pressure" detune. sfz files are CC0. | SFZ + FLAC | **315 MB** zip **V** | https://musical-artifacts.com/artifacts/5947/MelloSFZotron.zip (302 redirect to https://bucket.musical-artifacts.com/uploads/stored_file/file/9228/MelloSFZotron.zip) |
| NarwhalAudio/mellotron (one big mellotron.sfz plus sample_map) | SFZ + WAV | 519 MB | https://github.com/NarwhalAudio/mellotron/archive/refs/heads/main.zip |
| ExistentiaVirae/Mellotron-SFZ (23 .sfz files, one per tape set) | SFZ + WAV | 518 MB | https://github.com/ExistentiaVirae/Mellotron-SFZ/archive/refs/heads/main.zip |
| TaijiguyGigaTron (linuxsampler). **GIG format, so sfizz cannot load it.** | GIG | 313 MB **V** | http://download.linuxsampler.org/instruments/vintage/TaijiguyGigaTron.tar.bz2 |

Pink Floyd / 70s use: MkII Flute, MkII Violins, Combined Choir and 8 Choir are all in these sets.

## 4. Choir / "aah" voices

| Name | Licence | Format | Size | URL |
|---|---|---|---|---|
| **Sonatina Symphonic Orchestra 1.0**. Includes `Chorus - Female.sfz`, `Chorus - Male.sfz` and `Chorus - Mixed.sfz` (sustained "aah"), plus a full orchestra. | CC Sampling Plus 1.0 | SFZ + WAV | **462 MB** zip **V** | https://musical-artifacts.com/artifacts/81/sonatina-symphonic-orchestra-1.0.zip |
| Mellotron Combined Choir / 8 Choir (see section 3) | taijiguy (non-commercial) | SFZ | (in Mellotron set) | — |
| Dave Choir (Dave Hilowitz, 16x self-sampled, "ah"/"oh") | free | SFZ / Kontakt / DecentSampler | ? | https://decentsamples.com/?p=8646. The store checkout probably asks for an email, so it is **not a plain GET**. Get it manually if needed. |

Note: there is no truly high-end free SFZ choir. For a big Rammstein-style pad, layer SSO Chorus Mixed (male weight) with the Mellotron 8 Choir, then add slow attack, reverb and slight detune in the synth. Both VSCO-2 CE and VPO have essentially **no choir**.

## 5. Orchestral strings and brass

| Name | Licence | Format | Download | Unpacked | URL |
|---|---|---|---|---|---|
| **Virtual Playing Orchestra 3.2 wave files** (builds on SSO, VSCO2, NBO, Iowa and Philharmonia; sections and solos, many articulations) | Mixed CC sources. The author enforces no restrictions on music use, including commercial. | WAV | **616 MB** zip **V** | ~650 MB | https://ia801403.us.archive.org/18/items/virtual-playing-orchestra-3-2-wave-files/Virtual-Playing-Orchestra3-2-wave-files.zip (short link: https://virtualplaying.com/go/virtual-playing-orchestra-v3-2-wave-files-archive/) |
| VPO 3.3 Standard scripts (needed with the waves) | same | SFZ | 544 KB **V** | — | https://virtualplaying.com/vp-downloads/Virtual-Playing-Orchestra3-3-standard-scripts.zip |
| VPO 3.3 Performance scripts (optional; legato/performance variants) | same | SFZ | 357 KB | — | https://virtualplaying.com/go/virtual-playing-orchestra-v3-3-performance-scripts/ |
| **VSCO-2 Community Edition 1.1.0** (Versilian; 75 .sfz files, strings, brass, winds, percussion) | **CC0** | SFZ + WAV | ~3.1 GB zip (wav barely compresses) | **3.09 GB** (3,273 files) | https://github.com/sgossner/VSCO-2-CE/archive/refs/tags/1.1.0.zip **V** (200). The release has no binary asset; the tag zip is what the release notes point to. |
| Sonatina Symphonic Orchestra (see section 4) | CC Sampling+ | SFZ + WAV | 462 MB | — | (see section 4) |

Quality: VPO is the most playable "ensemble" choice and the smallest. VSCO-2 CE gives better true sections, is CC0, and is the cleanest to redistribute, but it is 3 GB.

## 6. Optional: organ, harpsichord, clavinet, drums

| Instrument | Name | Licence | Format | Size | URL |
|---|---|---|---|---|---|
| Hammond-style | FreePats Drawbar Organ Emulation | CC0 | SFZ (+FLAC) | 6.0 MB **V** | https://freepats.zenvoid.org/Organ/DrawbarOrganEmulation/DrawbarOrganEmulation-SFZ-20190712.tar.xz |
| Hammond-style | FreePats Percussive Organ Emulation | CC0 | SFZ | 12.4 MB **V** | https://freepats.zenvoid.org/Organ/PercussiveOrganEmulation/PercussiveOrganEmulation-SFZ-20190715.tar.xz |
| Rock organ | FreePats Rock Organ Emulation | CC0 | SFZ | 12.6 MB **V** | https://freepats.zenvoid.org/Organ/RockOrganEmulation/RockOrganEmulation-SFZ-20190715.tar.xz |
| Pipe organ | Orgue Eglise (S. Christian Collins conversion; Doux, Full, Reeds, Strings, keyswitch) | free (no explicit licence file) | SFZ + WAV | 29 MB | https://github.com/sfzinstruments/OrgueEglise/archive/refs/heads/master.zip |
| Harpsichord | Clavecin (S. Christian Collins conversion) | free (no explicit licence file) | SFZ + WAV | 5 MB | https://github.com/sfzinstruments/Clavecin/archive/refs/heads/master.zip |
| Harpsichord (better) | VCSL: 5 harpsichords, Steinway B, Kawai, uprights, pipe organ and more. **No .sfz in the repo; WAV only, so you write the mapping yourself.** | CC0 | WAV | 5.9 GB (whole repo) | https://github.com/sgossner/VCSL |
| Clavinet | Clavinet (SFZ, Lithalean) | "gray" | SFZ + WAV | 57 MB **V** | https://musical-artifacts.com/artifacts/646/Clavinet.zip |
| Wurlitzer | see section 2 (Greg Sullivan EP200) | CC-BY 3.0 | | | |
| Acoustic drums | **Big Rusty Drums 1.100** (Karoryfer; same as the commercial release) | **CC0** | SFZ + FLAC | **620 MB** **V** | https://github.com/sfzinstruments/karoryfer.big-rusty-drums/releases/download/v1.100/Big_Rusty_Drums_1100.zip |
| Acoustic drums | **Virtuosity Drums v0.925** | **CC0** | SFZ + FLAC | 1.227 GB **V** | https://github.com/sfzinstruments/virtuosity_drums/releases/download/v0.925/Virtuosity_Drums_v0.925.zip |
| Acoustic drums | MuldjordKit (DrumGizmo, FreePats) | CC-BY 4.0 | SFZ + FLAC | 165 MB **V** | https://github.com/freepats/muldjordkit/releases/download/2020-10-18/MuldjordKit-SFZ+FLAC-20201018.7z |
| Acoustic drums | DRSKit (DrumGizmo port, 42 .sfz files) | CC-BY 4.0 | SFZ + FLAC | 719 MB | https://github.com/sfzinstruments/DrumGizmo.DRSKit/archive/refs/heads/master.zip |
| LinnDrum | Linnoleum SFZ-1 v2 (LinnDrum Rev.2/Rev.3, tunable, hi-hat decay CC) | "various": sfz is free, LinnDrum ROM samples are a grey area | SFZ + WAV | 71 MB **V** | https://musical-artifacts.com/artifacts/6356/Linnoleum_SFZ-1-v2.zip |
| Retro drum machines | TicTokMen RetroDrums1 / Moogdrums1 | free (README: "free use licenses") | SFZ | 2 MB each | https://github.com/sfzinstruments/TicTokMen.RetroDrums1/archive/refs/heads/master.zip |
| Electronic (808-ish) | FreePats Synthesizer Percussion | CC0 | SFZ | small | https://freepats.zenvoid.org/Percussion/SynthesizerPercussion/SynthesizerPercussion-SFZ-20220718.7z |

No clean-licence real-808 SFZ was found. The CC0 option is the FreePats synth percussion, or synthesise the 808 in Faust; that is easy and better.

---

## Recommended sets

### Core set (about 3.0 GB download, about 3.2 GB on disk)

| # | Item | Download |
|---|---|---|
| 1 | Salamander Grand V3 SFZ+FLAC (freepats tar.gz) | 742 MB |
| 2 | jRhodes3c | 18 MB |
| 3 | Greg Sullivan E-Pianos (Wurli/CP80/Pianet) | 20 MB |
| 4 | MelloSFZotron (Mellotron flute/strings/choir) | 315 MB |
| 5 | Sonatina Symphonic Orchestra (choir + extra orchestra) | 462 MB |
| 6 | VPO 3.2 waves + 3.3 standard scripts (strings/brass) | 617 MB |
| 7 | FreePats Drawbar + Percussive + Rock organ | 31 MB |
| 8 | Clavecin + Orgue Eglise | 34 MB |
| 9 | Clavinet (Lithalean) | 57 MB |
| 10 | Big Rusty Drums (CC0) | 620 MB |
| 11 | Linnoleum LinnDrum | 71 MB |
| | **Total** | **≈ 2.99 GB** (≈ 2.5 GB if MuldjordKit at 165 MB replaces Big Rusty) |

### Extended set (adds about 5.5 GB, about 8.5 GB total)
- VSCO-2 CE 1.1.0, CC0 (3.1 GB): better orchestral sections.
- Virtuosity Drums, CC0 (1.23 GB).
- jRhodes3d, full-length samples (92 MB).
- Salamander 48k/24 WAV version (1.26 GB), only to avoid FLAC decode cost (normally not worth it).
- DRSKit (719 MB) or NarwhalAudio mellotron WAV (519 MB), as alternatives.
- VCSL (5.9 GB, CC0, WAV only, needs your own sfz mapping): for harpsichords and pipe organ.

### Licence summary for the app
- Redistributable without restriction: **CC0** (VSCO-2 CE, Big Rusty, Virtuosity, FreePats organs, VCSL, UprightKW) and **CC-BY** with attribution (Salamander, Greg Sullivan, MuldjordKit, DRSKit).
- **Non-commercial only**: jRhodes (BY-NC), all taijiguy Mellotron sets, SSO (Sampling+). These are fine for a personal app. Don't bundle them in a commercial product; have the app download them at first run instead.
- **Grey**: Lithalean Clavinet/Wurlitzer, Linnoleum (LinnDrum ROM). Use them personally only.

---

## Faust (grame-cncm/faust) for Windows

- **Latest release: 2.88.0**, published 2026-09-09. https://github.com/grame-cncm/faust/releases/tag/2.88.0
- Windows asset: **`Faust-2.88.0-win64.exe`**, an NSIS installer of **113.2 MB**:
  https://github.com/grame-cncm/faust/releases/download/2.88.0/Faust-2.88.0-win64.exe
- **No portable zip of the full Faust distribution** is published. The only Windows zip is `faustgen-1.83-win64.zip` (42.7 MB), which is the Max/MSP external, not the SDK. Other assets: macOS dmg files, `faust-2.88.0.tar.gz` (full source including the libraries, 81 MB), and `libfaust-ubuntu-{x86_64,aarch64}.zip` (Linux only).
  - Workaround: an NSIS installer can usually be unpacked without installing (`7z x Faust-2.88.0-win64.exe -oC:\faust`) to get a portable tree. This is not verified for this build.
- Contents, inferred from the packaging (`build/MakePkg.bat`: `cmake -DUSE_LLVM_CONFIG=on -DPACK=on -C backends/most.cmake -C targets/windows.cmake`, then `cpack -G NSIS64`):
  - The `most.cmake` backends are **CPP, LLVM, Interpreter and OldCPP**, enabled for COMPILER, STATIC and DYNAMIC builds. So **libfaust includes LLVM**.
  - `windows.cmake` turns on the executable, static lib, dynamic lib, and OSC + HTTPD static libs.
  - Install layout: `bin/faust.exe` (plus faust2xxx scripts), `lib/` holding `faust.dll` + import lib, static `libfaust.lib`, and `libfaustwithllvm.lib` (static libfaust with LLVM merged in; marked OPTIONAL in CMake, so check after install), and `libOSCFaust`/`libHTTPDFaust`.
  - `include/faust/...` is the complete architecture header tree: `dsp/poly-dsp.h`, `dsp/llvm-dsp.h`, `dsp/libfaust.h`, `dsp/interpreter-dsp.h`, `dsp/dsp.h`, `gui/*`, `audio/*`, `midi/*` and so on. `share/faust/` holds the `.lib` DSP libraries, examples, and the architecture .cpp files.
- Practical advice: for an embedded synth you need not ship libfaust at all. Compile `.dsp` to C++ ahead of time (`faust -a minimal.cpp -i` or `-cn MyDsp`) and include `faust/dsp/poly-dsp.h` from the installed `include/`. Use libfaust + LLVM only if you want hot-reloading `.dsp` at runtime.
