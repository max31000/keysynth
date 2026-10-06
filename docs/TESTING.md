# Testing, rendering and analysis

All commands from the repo root in PowerShell (Git Bash works too). Windows 11, MSVC Build Tools (VS 18 / 2026),
CMake ≥ 3.25, Python 3.12.

## Build

```powershell
scripts/configure.ps1                         # -> build/ (Visual Studio 18 2026, x64), deps cached in .deps/
scripts/configure.ps1 -BuildDir build-va      # parallel agents/worktrees: own build dir, shared .deps/
cmake --build build --config Release --parallel
cmake --build build --config Release --target keysynth-engine ks-render ks-bench ks-tests
```

Binaries land in `build/bin/<Config>/`. Options (pass via `-ExtraArgs '-DKS_RT_CHECKS=OFF'`):
`KS_BUILD_TESTS` (ON), `KS_RT_CHECKS` (ON in every config), `KS_WITH_SFIZZ` (ON: sfizz 1.2.3 for the `sampler`
module; `OFF` builds without it and the module is not registered).
`KS_WITH_FAUST` (ON; Faust from `KS_FAUST_DIR`, default `<main checkout>/.tools/faust`, see `docs/PLUGINS.md`).
Engine code builds with `/W4 /WX`; dependencies don't.
`configure.ps1` configures with `FETCHCONTENT_FULLY_DISCONNECTED=ON` (no network, shared sources untouched) when every
dependency source already exists in `.deps/` (`-Online` forces a connected configure, e.g. after a pin bump), holds an
exclusive lock on `.deps/.configure.lock` so concurrent configures wait for each other, and treats cmake stderr
warnings as output (only the exit code fails it).

## Launchers

```powershell
scripts/start.ps1              # play: builds ui/dist if missing/stale, engine (Release, real audio) + browser at :7340
scripts/start.ps1 -BuildDir build-int -Port 7351 -HttpPort 7350 -NoAudio -NoBrowser   # smoke test, side by side
scripts/start-dev.ps1          # UI work: engine --no-audio --no-midi + Vite dev server (:5173, ?engine=...)
```

Both wait for the port, open the default browser (unless `-NoBrowser`) and stop everything on Ctrl+C.
`-EngineArgs '--preset','<path>'` passes extra engine flags.

## Unit + render tests (Catch2)

```powershell
build/bin/Release/ks-tests.exe                # everything except hidden tags (~0.5 s)
build/bin/Release/ks-tests.exe "[full]"       # full render tier: {44.1, 48, 96} kHz x {32, 64, 512}
build/bin/Release/ks-tests.exe "[swap]"       # graph swapper incl. 10k-publish stress test
build/bin/Release/ks-tests.exe "[protocol]"   # protocol handlers without network
build/bin/Release/ks-tests.exe --list-tests
```

Tags: `[spsc] [params] [voice] [patch] [graph] [paths] [preset] [swap] [stress] [rt] [render] [protocol] [dsp]
[transport] [sequencer] [drums] [sampler] [plugins] [faust] [dll]`. `[faust]` tests SKIP when libfaust is missing; `[dll]` tests build plugins with
`scripts/build_plugin.ps1` (≈ 4 s, SKIP when MSVC is missing). Run the full tier before merging anything that touches DSP or presets.

### Sample-based presets (`sampler`)

`[sampler]` unit tests generate their own tiny WAV + SFZ fixtures (in `userdata/.test-sampler-<pid>/`) and never need
downloaded libraries. Factory presets using `sampler` are **skipped by the render suite** (they need GBs of samples)
and covered by the hidden `[samples]` test instead, which renders every one whose library is installed (skips the
rest) with the same criteria (drum kits: RMS > -50 dB, tail at least 20 dB below the overall RMS, since one-shot cymbals ring) and prints
load+render time per preset:

```powershell
build/bin/Release/ks-tests.exe "[samples]"            # needs python scripts/fetch_samples.py first
$env:KS_SAMPLES_FILTER = "piano,vpo"                  # optional: only presets whose path contains one of these
$env:KS_SAMPLES_DUMP = "renders"                      # optional: write each render to renders/<preset>.wav
```

Sample paths in presets are `assets/samples/<lib>/<entry>.sfz`, resolved against the repo root. A checkout without
its own `assets/samples` (e.g. an agent worktree) falls back to `$KS_ASSETS_DIR/samples/...` and then to the main
checkout's `assets/` (for roots laid out as `<main>/.claude/worktrees/<name>`), see
`engine/src/instruments/sampler/SamplePaths.h`. `KS_ASSETS_DIR` points at a directory laid out like `assets/`
(e.g. `$env:KS_ASSETS_DIR = "M:/Projects/Piano/assets"`). ks-render waits until every module reports ready
(samples loaded) before rendering.

Render suite (`engine/tests/test_render.cpp`): every `presets/factory/**` preset renders the standard pattern
(`OfflineRenderer::standardTestEvents()`: C-major chord, scale with rising velocity, low/high extremes, sustain-pedal
section) + 3 s tail and must satisfy: no NaN/Inf, peak ≤ 0 dBFS, |DC| < 1e-3, RMS > −40 dBFS (not silent), RMS of the
last 0.5 s < −80 dBFS (tails decay), and 0 real-time violations. A new preset is picked up automatically.

## Offline render (`ks-render`)

```powershell
build/bin/Release/ks-render.exe --preset presets/factory/synth-lead/basic-saw-lead.json `
    --notes "A3:0:1.5:100" --tail 1 --out renders/a3.wav
build/bin/Release/ks-render.exe --preset presets/factory/splits-layers/basic-bass-lead-split.json --test-pattern --out renders/split.wav
build/bin/Release/ks-render.exe --patch-json '@my_patch.json' --midi song.mid --sr 96000 --block 32 --out renders/song.wav
build/bin/Release/ks-render.exe --list-modules          # catalog JSON (ModuleInfo[], incl. plugins)
build/bin/Release/ks-render.exe --patch-json '@plugins/faust_pluck/demo.json' --test-pattern --out renders/pluck.wav
build/bin/Release/ks-render.exe --play-pattern presets/patterns/money-7-4.json --bars 2 --tail 1.5 --out renders/money.wav
```

Patches using `plugin:<name>` modules make ks-render compile/load those plugins from `plugins/` first (synchronously,
Faust machine-code cache in `plugins/.build/cache`); `--no-plugins` skips that, `--root DIR` picks another repo root.

`--cc "CC:value:time[:ramp],..."` adds controller automation (CC 0–119 or `mod`, `breath`, `expr`, `sustain`;
value 0–127; time in s; `ramp` s = linear ramp from the controller's previous value, reaching `value` at
`time + ramp`; ramps follow the `--cc` items only, not CCs from `--midi`), `--bend "value:time[:ramp],..."` pitch bend (−1..1 = the patch's full bend range). Use them to audition
mod-wheel behaviour offline (rotary slow/fast, vibrato, filter), e.g.
`ks-render --preset presets/factory/organ/b3-full-rock.json --notes "C4:0:4,E4:0:4,G4:0:4" --cc "mod:127:1,mod:0:3" --out renders/leslie.wav`.
Without `--notes/--midi/--test-pattern` the automation plays over the default C4.

`--play-pattern FILE --bars N [--tempo BPM]` plays a drum pattern through the patch's rhythm kit (pattern tempo, kit,
swing and meter applied; transport stops exactly after N bars, then `--tail`). Combine with `--notes`/`--midi` to
layer played notes on top.

`--notes "NOTE:start:dur[:vel],..."` — note name (`C4` = 60, `F#3`, `Bb2`) or number, times in seconds, velocity
1–127 (default 100). Without `--notes/--midi/--test-pattern` a single C4 is rendered; without `--preset/--patch-json`
the default patch (one `basic` layer). Output: 32-bit float stereo WAV (default `renders/out.wav`, gitignored).

## Loudness (`scripts/loudness.py`)

```powershell
python scripts/loudness.py --check                     # all factory presets on target? exit 1 if not
python scripts/loudness.py --filter organ              # measure a subset, print LUFS / dBTP
python scripts/loudness.py --apply --report docs/research/LOUDNESS.md   # renormalize master volume_db
```

Renders a standard phrase per category with ks-render (keys: bass note + chords + melody with sustain; leads, basses,
drums: their own phrase) and measures BS.1770-4 integrated loudness (LUFS) and 4x-oversampled true peak. Target
−16 LUFS ±1, ≤ −1 dBTP; only the master `volume_db` is changed (table and exceptions: `docs/research/LOUDNESS.md`).
Run `--check` (with `$env:KS_ASSETS_DIR` for sampler presets; missing libraries are skipped) after adding or
changing a factory preset or anything that changes the level of an engine/effect, and `--apply` for new presets
(≈ 2–5 min for all with `--jobs 8`; `--ks-render PATH` picks the binary, default the first `build*/bin/<cfg>/`).

## Analysis (`scripts/analyze_wav.py`)

```powershell
python scripts/analyze_wav.py renders/a3.wav --start 0.2 --end 1.2 --spectrogram renders/a3.png
python scripts/analyze_wav.py renders/a3.wav --json
```

Prints peak/RMS/DC per channel, fundamental (autocorrelation, with note name + cents), spectral centroid, envelope
summary (peak, attack, time above −60 dB rel., tail level, RMS per 250 ms). Installs numpy/scipy/matplotlib with pip
if missing. Use `--start/--end` to measure pitch on the sustained part of a note.

## Benchmark (`ks-bench`)

```powershell
build/bin/Release/ks-bench.exe                          # 48 kHz / 64, 10 s per factory preset
build/bin/Release/ks-bench.exe --sr 96000 --block 32 --seconds 20 --filter split
```

Prints the realtime factor per preset; exit code 1 if the worst preset has < 2x headroom (ARCHITECTURE §12).

`ks-bench --latency [--sr HZ] [--block N]` instead prints, per registered instrument engine (default params, one
layer, whole graph via OfflineRenderer; `sampler` uses the first factory preset whose library is installed), the
samples/ms from a C4 note-on at block offsets 0 and 37 to the first output sample above the pre-note noise floor
(≥ 1e-6); exit code 1 if any engine needs more than one block. Same check in `engine/tests/test_note_latency.cpp`
(`[latency]`; fm ≤ 8 samples; sampler in the hidden `[samples]` tier).

## Running the engine headless

```powershell
build/bin/Release/keysynth-engine.exe --no-audio           # null device (timer thread), for UI/protocol work
build/bin/Release/keysynth-engine.exe                      # real audio: ASIO Steinberg/Yamaha if present
build/bin/Release/keysynth-engine.exe --asio-buffer 64 --preset presets/factory/synth-lead/basic-saw-lead.json
build/bin/Release/keysynth-engine.exe --port 7351 --http-port 0 --no-midi   # side-by-side instance, no static UI
```

Flags: `--no-audio`, `--no-midi`, `--no-plugins` (don't load/watch `plugins/`), `--port N` (WebSocket, default 7341), `--http-port N` (static `ui/dist`, default
7340, 0 = off), `--asio-buffer N`, `--sample-rate HZ`, `--preset PATH`, `--root DIR`. On start it prints device,
sample rate, buffer, device-reported latency and (after the first callback) the MMCSS / power-throttling state.
The chosen device is saved to `userdata/settings.json`; delete it to return to auto-selection. Note: ASIO drivers
may only accept the buffer size set in their own control panel (the UR22C driver did: 1024 regardless of
`--asio-buffer`; it offers only `[1024]`, printed as "buffer sizes offered by the driver") — change it there, from the
UI's "Open ASIO panel" button (protocol `open_audio_panel`, PROTOCOL.md *Audio latency*). Quit with Ctrl+C.

Quick protocol smoke test (Python, `python -m pip install websockets`):

```python
import asyncio, json, websockets
async def main():
    async with websockets.connect("ws://127.0.0.1:7341") as ws:
        await ws.send(json.dumps({"type": "hello", "id": 1, "client": "ui", "version": "0"}))
        print(json.loads(await ws.recv())["type"])                     # state
        await ws.send(json.dumps({"type": "get_catalog", "id": 2}))
        print([m["typeId"] for m in json.loads(await ws.recv())["modules"]])
        await ws.send(json.dumps({"type": "note", "on": True, "note": 60, "velocity": 100}))
        for _ in range(10): print(json.loads(await ws.recv())["type"])  # midi / telemetry events
asyncio.run(main())
```
