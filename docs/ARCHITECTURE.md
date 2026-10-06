# keysynth — Architecture

Source of truth for structure and invariants. If code and this doc disagree, fix one of them in the same change.
Revision 2 (after review #1: graph swap semantics, stable node ids, per-device MIDI queues, RT enforcement).

## 1. Goal and non-goals

Goal: MIDI keyboard (USB) → low-latency software synth → ASIO audio interface. Many built-in engines and presets,
user-extensible DSP (hot-reloaded plugins), web UI.

Non-goals (for now): hosting third-party VST3, DAW timeline, MIDI controller mapping, sidechain, MPE (API is ready
for it: `noteId`), plugin builds of keysynth itself.

Platform: Windows 11 + ASIO first. Code stays portable: no Win32 calls outside `engine/src/platform/`.

## 2. Process topology

```
 ┌──────────────── keysynth-engine (C++20, JUCE 8, headless) ────────────────┐
 │  MIDI device threads ─► one SPSC per device ─┐                             │
 │  Control thread ──────► ControlInbox (SPSC) ─┼─► AUDIO THREAD             │
 │                                              │   Engine::process()        │
 │  Control thread ◄── telemetry (atomics + event SPSC) ◄─┘  RackGraph       │
 │      ├── ControlServer: WebSocket ws://127.0.0.1:7341 (JSON) + HTTP ui/dist│
 │      ├── PatchModel (single source of truth for the patch)                 │
 │      └── PresetStore, SettingsStore, SampleLibrary, PluginHost (Faust/DLL) │
 └────────────────────────────────────────────────────────────────────────────┘
                    ▲ WebSocket JSON
 ┌──────────────── ui (TypeScript, React, Vite) ──────────────────────────────┐
 │ Browser tab. Never on the audio path. Pure remote control.                 │
 └────────────────────────────────────────────────────────────────────────────┘
```

The UI is never on the sound path (latency = MIDI in → audio buffer only), so a web UI costs no latency, is cheap to
iterate, testable in a browser, and generated from parameter metadata.

Threads:
- **Audio thread** (ASIO callback). Real-time. Rules in §4.
- **MIDI device threads** (JUCE callbacks, one per open input). Each only pushes into its own SPSC queue.
- **Message/control thread** (JUCE message loop). Owns all non-RT state: PatchModel, graph building, presets,
  server, file IO, plugin loading. WebSocket library threads hand every message to this thread
  (`juce::MessageManager::callAsync`) — never touch engine state from network threads.
- **Loader threads** (`juce::ThreadPool`, below-normal priority) for samples; PluginHost has its own loader thread
  for plugin compilation (§10). Results return to the message thread.

## 3. Repository layout

```
CMakeLists.txt            options KS_BUILD_TESTS, KS_WITH_SFIZZ (ON), KS_RT_CHECKS (default ON in all configs, see §4.8)
                          KS_WITH_FAUST (ON; headers from KS_FAUST_DIR = <main checkout>/.tools/faust, §10)
cmake/Dependencies.cmake  FetchContent, every dep pinned to a tag/commit: JUCE (>=8.0.11, bundles ASIO),
                          nlohmann_json, ixwebsocket (USE_TLS=OFF, no zlib), Catch2 v3, sfizz (isolated target,
                          warnings off). CMAKE_MSVC_RUNTIME_LIBRARY set globally.
third_party/msfa/         DX7 FM core vendored from a pinned Dexed commit (Apache-2.0, LICENSE kept).
engine/
  src/core/       Module API, ParamSpec/ParamSet, ModuleRegistry, PatchModel, GraphBuilder, RackGraph,
                  GraphSwapper, SpscQueue, MidiEvent, ChannelState, VoiceAllocator, Telemetry, RtCheck.
  src/dsp/        Header-mostly DSP primitives (PolyBLEP osc, ladder/SVF, ADSR, LFO, smoothers, delay lines,
                  BBD, IIR oversampler, noise, saturators, tables). No allocation in process.
  src/instruments/<engine>/   One dir per instrument engine.
  src/effects/<effect>/       One dir per effect.
  src/transport/  Transport clock, Metronome, DrumSequencer, (later) Looper.
  src/preset/     Patch data model <-> JSON, Migrations, PresetStore.
  src/control/    ControlServer (WS + static HTTP via ix::HttpServer), protocol handlers, snapshots.
  src/audio/      AudioHost (devices, ASIO, settings), MidiHub (all MIDI inputs).
  src/plugins/    PluginHost: Faust JIT (libfaust C API) + C ABI DLLs (sdk/include/keysynth/plugin_abi.h), hot reload.
  src/render/     OfflineRenderer (patch + notes/MIDI → buffer/WAV without a device).
  src/platform/   OS specifics (MMCSS "Pro Audio", power throttling, SEH wrapper).
  src/app/        main.cpp — wiring only.
  tools/          ks-render (offline render), ks-bench (CPU per preset).
  tests/          Catch2 tests.
sdk/include/keysynth/plugin_abi.h   Stable C ABI for DSP plugins.
plugins/<name>/   Plugin sources (plugin.json + .dsp or .cpp). Built to plugins/.build/ (gitignored).
presets/factory/<category>/   Factory patches.   presets/patterns/  Drum patterns.
userdata/         (gitignored) user presets, settings.json, recordings.
assets/samples/   (gitignored) sample libraries; assets/samples.json manifest (committed).
ui/               Vite + React + TS.
scripts/          configure.ps1, fetch_samples.py, fetch_faust.ps1, build_plugin.ps1, analyze_wav.py.
docs/             ARCHITECTURE, PROTOCOL, PRESETS, PLUGINS, TESTING, STATUS, DSP_NOTES.
```

Sources under `engine/src/**` are globbed (`CONFIGURE_DEPENDS`) — adding a module never edits CMake. Modules are
registered explicitly, one line each, in `engine/src/core/BuiltinModules.cpp`.
`ks_core` static lib = everything except app/tools; linked by `keysynth-engine`, `ks-render`, `ks-bench`, `ks-tests`.
All code is testable without an audio device.

## 4. Real-time rules (audio thread)

1. No heap allocation/free, no locks, no file/network IO, no logging, no exceptions, no `std::function`
   construction, no `shared_ptr` copies/drops (audio thread holds raw pointers only).
2. Buffers sized in `prepare(sampleRate, maxBlock)`; voices preallocated (fixed max polyphony per engine).
3. `process` accepts any `n ≤ maxBlock`; the Engine splits larger device callbacks into ≤ maxBlock chunks.
4. Communication only via: atomics, SPSC queues (`core/SpscQueue.h`), the GraphSwapper slot.
5. `juce::ScopedNoDenormals` at the top of the callback (re-asserted after plugin calls). No NaN/Inf for any param
   value within spec range (tests sweep params).
6. Bounded cost per block.
7. **Zero added latency**: built-in modules have `latencySamples()==0`. Master limiter is zero-lookahead (soft clip +
   fast release). Oversampling only via min-phase IIR polyphase, never linear-phase FIR.
   Allowed exception: the 2-sample PolyBLEP/BLAMP oscillators (`dsp/BlepOsc.h`, used by `va`) output the
   waveform one sample late (21 us at 48 kHz) so residuals can be applied on both sides of a discontinuity.
   This is below any audible/MIDI-jitter threshold and is not reported as `latencySamples()`.
8. **Enforcement** (`KS_RT_CHECKS`, on in Debug/tests): `RtScope` sets a thread_local flag inside
   `Engine::process`; replaced global `operator new/delete` and the `ks::Mutex` wrapper assert when it is set.
   A swap stress test does 10k random publishes during offline render.
   Violations are *counted* (`rt::violationCount()`, also in telemetry as `rtViolations`); tests assert 0;
   `KS_RT_ABORT=1` aborts on the first one. KS_RT_CHECKS defaults ON in every config (cost: one thread_local read
   per allocation) so the Release test run is checked too; turn it off for shipping builds.

## 5. Core model

### 5.1 Parameters

```cpp
enum class ParamScale { Linear, Log, Int, Enum, Bool };
enum ParamFlags : uint32_t { None = 0, ReadOnly = 1, Hidden = 2, NonAutomatable = 4 };
struct ParamSpec {
  std::string id;      // stable snake_case, unique in module — part of preset format
  std::string name, group, unit;
  float min, max, def;
  ParamScale scale;
  float skewCentre;    // Log params: value at knob centre (0 = geometric mean)
  uint32_t flags;
  std::vector<std::string> choices; // Enum labels; value = index
};
```

- `ParamSet` = fixed array of `std::atomic<float>` (plain units). Control thread writes, audio thread reads
  once per block; modules smooth where audible.
- `ReadOnly` params are written by the audio thread (meters: compressor GR, rotary speed, current DX7 voice name
  index…) and polled into telemetry at 30 Hz.
- Renaming an id = breaking; add a migration in `preset/Migrations.cpp`.
- Optional `uiHints` JSON per module (group order, front-panel params, knob/slider). UI is generic.

### 5.2 Modules

```cpp
enum class ModuleKind { Instrument, Effect };
class Module {
 public:
  virtual ~Module() = default;
  virtual const ModuleInfo& info() const = 0;        // typeId, displayName, kind, category, params, uiHints
  ParamSet& params();
  virtual void prepare(double sampleRate, int maxBlock) = 0;  // control thread, before going live
  virtual void reset() = 0;                          // kill voices/tails
  // Audio thread. Instrument: buffer cleared on entry. Effect: in-place stereo.
  virtual void process(AudioBlock& stereo, MidiEventSpan events, const ProcessContext& ctx) = 0;
  virtual int tailSamples() const { return 0; }
  virtual int latencySamples() const { return 0; }
  virtual nlohmann::json saveState() const { return {}; } // non-param state (sample path, syx bank…)
  virtual void loadState(const nlohmann::json&) {}        // control thread, before prepare
  virtual bool loadStateWroteParams() const { return false; } // loadState wrote params (fm .syx voice)
  virtual std::string loadError() const { return {}; }    // control thread: failed resource load (bad sfz/syx)
  virtual bool isReady() const { return true; }           // false while heavy resources load asynchronously
  virtual void setOfflineMode(bool) {}                    // control thread, after prepare: faster-than-RT render
};
```

- **Asynchronous loading** (`isReady`): a module whose `prepare()` starts a background load (samples) renders
  silence and returns `false` until it is done; it never blocks the audio or message thread on it. `OfflineRenderer`
  (ks-render, tests) calls `setOfflineMode(true)` on every module and waits until all are ready before rendering
  (`RenderOptions::readyTimeoutSeconds`). In offline mode a streaming module may wait for disk IO inside `process()`
  (sfizz freewheeling) — that is the only place such waits are allowed, inside an explicit `rt::RtAllowScope`.
  Both hooks default to no-ops, so existing modules are unaffected.
- **State that writes params** (`loadStateWroteParams`): GraphBuilder applies patch params *before* `loadState`, so a
  state that sets params (fm `{syx, voice}`) would override later edits on every rebuild. Such a module marks its
  state consumed (fm: `"applied": true`, then `loadState` skips the file) and GraphBuilder returns its params + new
  state in `BuildResult::adopted`; `Session::rebuild` copies them into the patch (`PatchModel::adoptModuleState`, not
  dirty) and the built node already carries the new state, so the next build reuses the module.
- **Load errors** (`loadError`): polled by `Session::pollModuleErrors` on the 30 Hz pump and reported once per module
  instance as a `log` error event (PROTOCOL.md); `OfflineRenderer` adds them to its warnings (ks-render prints them).

- `ProcessContext`: sampleRate, numSamples, sampleTime, transport (tempo, ppq position, playing), and per-layer
  `ChannelState` {pitchBend (-1..1), modWheel, aftertouch, cc64 sustain, cc66 sostenuto, cc67 soft, expression}
  maintained by the core (engines don't parse CCs, but still receive raw events). The Engine keeps 17 global
  states (0 = omni merge, 1..16); each layer gets a copy for its zone channel with sustain/sostenuto masked when
  `zone.sustain` is false.
- Module helpers beyond the sketch above: the base constructor takes the module's static `ModuleInfo` (so the
  ParamSet can be built), `info()` is virtual-with-default, `activeVoices()` feeds telemetry, and
  `Module::liveInstances()` counts instances for leak tests.
- `MidiEvent` POD: {sampleOffset, type, channel, note/cc, value7, valueF, noteId (int32, MPE/MIDI2-ready)}.
- `ModuleRegistry`: typeId → {ModuleInfo, factory}. Built-ins via `BuiltinModules.cpp`; plugins at runtime
  (`plugin:<id>`). UI gets the catalog through the protocol.
- `VoiceAllocator<VoiceT, N>` (core): poly/mono/legato, stealing (released first, then oldest), sustain/sostenuto,
  glide. Unison stays inside engines.

### 5.3 Patch, PatchModel and RackGraph

```
Patch (data)                              RackGraph (live, audio thread)
 ├─ layers[] {nodeId, zone, instrument{nodeId,type,params,state}, fx[]{nodeId,type,params,state,bypass}}
 ├─ master {volume_db, fx[]}               ├─ LayerNode[] (zone filter, instrument, fx chain, gain/pan, meters)
 ├─ rhythm {drums kit params, pattern}     ├─ RhythmNode (drums instance for the sequencer + metronome voice)
 └─ meta                                   └─ master fx → (+ metronome) → safety limiter
```

- Every layer and module slot has a stable `nodeId` (uint32, assigned by PatchModel, persisted in the patch).
  Params are addressed as `{nodeId, paramId}` — never by index.
- **PatchModel** (control thread) is the single source of truth. Protocol handlers only call PatchModel methods.
  A param write updates PatchModel and then the atomic in the latest *published* graph (lookup table
  nodeId → ParamSet*, rebuilt on publish). GraphBuilder reads params from PatchModel at publish time, so writes
  made during a build are never lost.
- Signal flow: MIDI (all inboxes merged per block) → ChannelState update → Transport/DrumSequencer → each LayerNode
  zone filter (key/vel range on physical key, then transpose, channel) → instrument → layer FX → sum → master FX →
  master `volume_db` → trailing master `limiter` slot(s), if any → + metronome → safety limiter → device. The master
  volume goes *before* the limiters, so a preset's own limiter (and always the safety limiter) stays last; the
  range is −96..+12 dB, out-of-range values are clamped with a load warning (protocol `log`).
- The metronome and the safety limiter are owned by the Engine's output stage, not by RackGraph: they persist
  across swaps and are applied once to the mixed output of both graphs during a transition.
- The Engine's maxBlock = device buffer size (offline: `--block`); larger callbacks are split.
- `Transport` (tempo, play state, position) lives in the Engine, outside the graph.
- **RhythmNode** = a `drums` ModuleNode (stable `nodeId` from `patch.rhythm.node`, addressable by `set_param`, reused
  across rebuilds like any module, render-once shared in transitions). The Engine's DrumSequencer generates its
  note events per block (`RenderArgs.rhythmEvents`, sample offsets inside the block); the graph renders it into its
  own buffer, applies the per-block `drums_volume` gain ramp and sums it with the layers *before* master FX. During
  a transition the old graph's RhythmNode gets no sequencer events (only the AllNotesOff).

### 5.4 Structural changes (GraphBuilder + GraphSwapper)

- Structural change (patch load, add/remove layer/FX, change instrument type/state) → GraphBuilder builds a new
  RackGraph from PatchModel on the control thread (construct, `loadState`, `prepare`), then `publish()`.
- **Module reuse.** Modules are owned by `shared_ptr` inside graphs (refcounts only touched on the control thread).
  GraphBuilder diffs against the current graph: a module whose `{nodeId, type, state}` is unchanged and whose
  sampleRate/maxBlock match is *shared* into the new graph, not rebuilt (keeps held notes, sfizz sample pools,
  reverb tails). Effects are reused the same way.
- **Swap.** Single `pending` atomic slot. `publish()` exchanges into `pending`; if the exchange returns a graph the
  audio thread never took, the control thread deletes it. The audio thread takes `pending` only when no transition
  is running → at most 2 live graphs. When it takes a graph it copies each layer's sounding-note map (physical key →
  transposed note) from the outgoing live graph, so note-offs of held keys reach a shared instrument even if the
  graph was built before the key went down (or from a pending graph that never ran).
- **Transition.** Modules shared by both graphs are processed once per block (render-once cache keyed by module
  pointer; output feeds both graphs' downstream). If nothing is shared (preset switch), the old graph gets
  all-notes-off and keeps rendering until silent or `min(tailSamples, 1 s)`, mixed with the new one; a new pending
  graph arriving during that time forces a 10 ms fade-out of the old graph. ChannelState (sustain, mod, bend) is
  global so it carries over.
  Implementation: "shared" transitions are a 20 ms linear crossfade (old chain → new chain; shared modules
  rendered once, so a shared instrument is not doubled); unshared transitions tail out for
  `clamp(maxTail, 50 ms, 1 s)` or until 2048 silent samples, then fade 10 ms. Preset loads build with no reuse
  (node ids of unrelated patches must not share modules); edits of the current patch reuse.
- **Retire.** Old graph pointer → retire SPSC (capacity 8). If push fails, the audio thread keeps it and retries next
  block; it never frees. Control thread drains on a 50 ms timer and deletes.
- Device/sample-rate/buffer change: stop device → rebuild graph with new prepare args (no reuse) → restart.
- Heavy resources (samples, syx banks, DLLs) are loaded on loader threads and cached ref-counted in
  `SampleLibrary`/`PluginHost`; released only on the control thread.

### 5.5 Telemetry (audio → control)

- Continuous values (meter peaks/RMS per layer + master, voice counts, CPU load = callback time / buffer duration,
  ReadOnly params) are latest-value atomics, polled at 30 Hz.
- Discrete events (MIDI note activity, xruns) go through an SPSC; when full they are dropped, never block.

## 6. Audio & MIDI host

- `AudioHost` wraps `juce::AudioDeviceManager`. Default: ASIO device matching "Steinberg"/"Yamaha" if present, else
  system default. Device type/name, sample rate, buffer size, output channels persisted in `userdata/settings.json`,
  switchable at runtime. Reports device-reported input/output latency + buffer to the UI.
  ASIO drivers may accept only the buffer size set in their own control panel; `open_audio_panel` shows it
  (message thread). A modal panel runs a nested message loop, so requests are re-entrant meanwhile:
  `AudioStatus::panelOpen` is set while it shows and the host refuses a second panel / device switch (`busy`) and
  skips driver rescans. Any device restart (panel change → driver reset request, or setDevice) runs
  `audioDeviceAboutToStart` → Engine::prepare + rebuild, then asynchronously saves settings and fires
  `AudioControl::onChanged` (app: broadcast `devices` + `state`; setDevice reports itself instead).
  Details: PROTOCOL.md *Audio latency*.
- On the first callback `platform/` registers the audio thread with MMCSS (`AvSetMmThreadCharacteristicsW
  "Pro Audio"`) and disables power throttling for it (Intel hybrid P/E cores).
- `MidiHub` opens all MIDI inputs, one SPSC per device; rescan on demand (protocol `rescan_midi`) and at startup.
  Events get sampleOffset 0 at the next block (lowest latency; jitter ≤ 1 block).
- UI virtual keyboard / computer keyboard notes enter through ControlInbox → same MIDI path.
- `--no-audio`: a timer thread drives `Engine::process` in real time without a device, for UI/protocol testing.

## 7. Engines (v1)

| typeId | What | Covers |
|---|---|---|
| `va` | Virtual analog poly (16 notes): 3 PolyBLEP/BLAMP osc (saw/pulse+PWM/tri/sine/JP-8000 supersaw/noise), sub −1/−2 oct, hard sync 2→1, ring 1×2, FM 3→1, filter-env→osc1 poly-mod; filters Moog ladder 24 / IR3109 24 (ZDF, nonlinear, self-osc) / SEM 12 SVF, LP/BP/HP/notch, HPF; analog ADSRs, 2 LFO (delay, key/tempo sync), 6-slot mod matrix; unison ≤8 within a 64 sub-voice budget, glide (constant time, legato-only), poly/mono/legato, drift, pan spread; output saturation | Juno/Jupiter/OB-Xa/Prophet/Minimoog/CS-80: Take On Me, Jump, Floyd leads, Rammstein pads/brass/supersaw |
| `fm` | DX7-compatible 6-op FM via vendored MSFA (Apache-2.0, Dexed fork; engine models Modern / Mark I / OPL, the latter two GPL-3.0+): every DX7 voice param as a ParamSpec (`alg`, `feedback`, `op1_level`, `op1_eg_rate1`…), macros (brightness, attack/release, tune, voices), DX7-style wheel/aftertouch routing; state `{syx, voice}` loads a voice from a 32-voice bulk or single dump into the ParamSet. Renders 8-sample MSFA chunks ahead (msfa `LG_N` = 3, Dexed uses 64; latencySamples 0, events ≤7 samples late, onset tested ≤8); one sample rate per process (msfa globals) | DX7 E.Piano, bells, basses, brass |
| `organ` | Tonewheel wheel-bus: 91 wheels (B-3 gear ratios), 9 drawbars with manual foldback, single-trigger percussion, key click, scanner vibrato/chorus V1–C3, leakage, preamp drive; pairs with `rotary` | Hammond B3 (Floyd: Echoes, Time) |
| `combo` | Transistor combo organ: divide-down (12 masters + dividers), Vox Continental / Farfisa voicings, footages/tabs, formant filters, vibrato, bass section | Doors: Light My Fire; early Floyd |
| `epiano` | Modal EP, no samples (8 modes/voice, `dsp/ModalBank` + `dsp/BeamModes`): alpha-pulse hammer → coupled tine/tonebar normal modes + clamped-free overtones (strike position) → magnetic pickup d/dt 1/(1+u²) (alignment/distance → bark, tine buzz) or Wurlitzer electrostatic pickup + preamp; felt dampers, continuous CC64 half-damper, re-strike, ghost-faded stealing, 32 voices. Models: Rhodes Mk I / Mk II / Suitcase (stereo vibrato) / Wurlitzer 200A / Piano Bass (timbre of E1–B3) | Riders on the Storm, Money/Breathe, Supertramp |
| `sampler` | SFZ via sfizz 1.2.3 (isolated static target, `KS_WITH_SFIZZ`). State `{"sfz": path}`; per-instance loader thread (loads serialized across instances; also does non-RT voice reallocation via a lock-free pause handshake), silence + `loading`=1 until ready; sample pool per instance (no cross-instance cache yet); params volume/pan/transpose/tune/polyphony (non-automatable: cuts notes)/velocity curve; the `.sfz` path must resolve inside the §11 roots, or below `$KS_ASSETS_DIR` / the main checkout's `assets/` (`instruments/sampler/SamplePaths.h`); paths *inside* the SFZ (`sample=`, `#include`, `default_path`) are followed by sfizz unchecked | Salamander grand, Rhodes/Wurli/CP80, Mellotron, SSO/VPO choir, strings, brass, harpsichord, organ, clavinet, drum kits |
| `drums` | Synth kit, 13 voices (kick, snare, clap, closed/open hat w/ choke, crash, ride, 3 toms, rim, cowbell, tambourine), each with model 808/909/Linn/Industrial (`kit` default or per-voice `<v>_model`) + level/tune/decay/tone/pan; GM key map, other octaves fold onto 36–47; keys-playable; used by the RhythmNode | 80s beats, Rammstein stomp |

Effects v1: `chorus` (Juno BBD I/II/I+II, custom, Dimension), `ensemble` (string-machine 3-phase), `phaser`
(4/6/8/12 stages), `flanger` (BBD, optional through-zero), `delay` (stereo/ping-pong/tape, tempo sync), `reverb`
(16-line FDN hall/plate/room/chamber/gated/shimmer), `drive` (tube/fuzz/tape, IIR-oversampled 2x/4x), `rotary`
(Leslie 122/147 horn+drum, Doppler/AM, ramped slow/fast/brake, mod-wheel speed, ReadOnly `horn_rpm`/`drum_rpm`),
`tremolo` (amp trem with L/R phase / equal-power autopan; sine/tri/smoothed square; free or tempo-synced),
`compressor` (VCA/opto, `gr_db` meter), `eq` (HP/LS/3 bells/HS/LP), `limiter` (master safety, fixed).
Effects are in-place stereo, zero latency, smoothed.
Shared effect param ids: `mix` (0..1 dry→wet crossfade; 0 = dry, bit-exact except `drive`, whose dry runs
through the matching all-pass oversampling filters), `rate` (Hz), `depth` (0..1), `feedback`, `time` (ms),
`sync` (the one tempo-sync convention: an enum over `dsp/NoteDivision.h`, index 0 = Off → free `rate`/`time`,
1.. = 4/1, 2/1, 1/1, 1/2, 1/2., 1/2T, … 1/32T at the ProcessContext tempo; used by `delay`, `phaser`, `flanger`,
`chorus` (Custom/Dimension), `tremolo` and the `va` LFOs as `lfo1_sync`/`lfo2_sync` — no separate on/off or
`division` params; the order is a format surface, patch format 1 → 2 migrated in `preset/Migrations.cpp`), `tone`
(0..1), `width` (0..1), `level_db`; other dB/ms/Hz ids carry the unit as suffix (`_db`, `_ms`, `_hz`); `decay`
is RT60 in s. Accepted exception to §4.7: `flanger` `through_zero` (off by default) replaces the dry path with a
reference line of `time` (≤ 10 ms) — the effect itself, `latencySamples()` stays 0. BBD/delay/oversampling/filter
primitives live in `dsp/` (Bbd, InterpDelay, HalfbandIir, ...).

## 8. Transport

Engine-owned: tempo, time signature, play/stop, ppq position, count-in, swing, drums on/volume (atomics written by
the control thread, read once per block).
- `Transport`: position = anchor ppq + samples since anchor × tempo (re-anchored on tempo change), so step times do not
  drift with block size.
- `Metronome`: synthesized click on every denominator beat, downbeat accent, volume; forced on during the count-in
  bar; mixed after master FX, before limiter.
- `DrumSequencer`: up to 16 tracks × 256 steps (`bars × num × steps_per_beat`), per-step velocity, accent row, track
  mute, swing; patterns in `presets/patterns/*.json` (schema PRESETS.md). Control → audio via a preallocated
  **triple buffer** of fixed-size `RtPattern` PODs (no allocation, no locks; the audio thread picks the newest at block
  start, so a pattern swap while playing is seamless and keeps the position modulo the new length; a different step
  length re-anchors at the next step). Step k fires at the first sample ≥ its exact ppq time (ε = 1e-4 samples),
  computed per block from the block-start ppq — sample-accurate for any block size. Note-ons only (drum voices are
  one-shots), channel 10.
- `Looper`: later (not v1).

## 9. Presets

Schema in `docs/PRESETS.md` (`"format": 2`). Lenient loading: unknown params ignored, missing = default.
Factory presets `presets/factory/<category>/<slug>.json` (read-only); user presets `userdata/presets/`.
Categories: Piano, E.Piano, Organ, Synth Lead, Synth Pad, Synth Bass, Brass, Strings, Bells & Keys, Choir & Vox,
Drums, FX, Splits & Layers. Signature presets name their reference in `description`.

## 10. Plugins (hot-reloaded DSP)

User guide and conventions: `docs/PLUGINS.md`. Code: `engine/src/plugins/`, OS bits in `platform/DynamicLibrary`.

- **Sources.** `plugins/<name>/` (name `[a-z][a-z0-9_]{0,63}`, = id; typeId `plugin:<name>`) contains either
  `<name>.dsp` (Faust, optional `plugin.json`) or `<name>.cpp` (C ABI DLL). Build output, Faust machine-code cache
  and objects live in `plugins/.build/` (gitignored).
- **Faust path (primary).** libfaust (LLVM backend) compiles the `.dsp` in-process on the loader thread
  (`createCDSPFactoryFromFile`, `-I share/faust -I <plugin dir>` + white-listed `faust_options`, LLVM opt level
  max). Factories are cached as machine code in `plugins/.build/cache/<name>-<key>.fmc`, key = hash of libfaust
  version, LLVM target, options and every `.dsp/.lib` in the plugin dir. Only libfaust's **C API** is used, loaded
  at runtime from `<KS_FAUST_DIR>/lib/faust.dll` (function table, no import lib): faust.dll uses the dynamic CRT,
  keysynth the static one, so no C++ objects may cross. Factory create/read/write/delete hold one mutex (a factory released
  while a compile runs is queued and deleted later, so the control thread never waits for a compile); instance
  create/delete run on the control thread, compute/clear on the audio thread.
  - Instruments (0 inputs, 1–2 outputs): keysynth's own polyphony (not Faust's `mydsp_poly`, which is C++ API and
    path-string based): N instances (`polyphony` from plugin.json or `[nvoices:N]`, ≤ 32), core `VoiceAllocator`
    (stealing, sustain/sostenuto). Voice controls by label: `freq`/`key`, `gain`/`vel`/`velocity`, `gate`; bend
    ±2 st applied to `freq` per block. Re-strike/steal drops `gate` to 0 for one sample (envelope retrigger). A
    voice ends when gate is off and its output stays < −90 dBFS for 2048 samples, or after `max_release_s`.
  - Effects (1–2 in, 1–2 out): one instance; 1→1 runs dual mono (two instances); inputs are copied to scratch
    (no aliasing, `-vec` safe).
  - Params: `[id:x]` metadata, else snake_case of the path below the root group; invalid/duplicate ids reject
    the plugin. Full Faust UI → ParamSpec mapping: `docs/PLUGINS.md` "Parameters". Values are written to every
    instance's zones once per block.
  - RT: Faust's generated compute never allocates (state lives in the instance); `ks-tests` asserts the LLVM IR
    of the example plugins contains no allocator calls.
- **C ABI path (heavy C++ DSP).** `sdk/include/keysynth/plugin_abi.h`: `ks_get_plugin()` → descriptor
  {struct_size, abi_version, id, name, category, kind, param specs, create, destroy, prepare, reset,
  process(float** stereo, nframes, events, nevents), set_param, get_param (ReadOnly), tail_samples,
  latency_samples, save_state/load_state (blob)}. `ks_event` is layout-identical to `MidiEvent` (static_assert, no
  copy). `set_param` is called on the audio thread at block start only when the value changed (all values once
  after `prepare`). `scripts/build_plugin.ps1 <name>` compiles with MSVC (/MT, /O2) into
  `plugins/.build/<name>-<hash>.dll` (temp name + rename). The host loads the newest `<name>-*.dll` through a copy
  `%TEMP%/keysynth-plugins/<pid>/<name>-<hash>-<n>.dll` (so builds can overwrite/delete; only directories of dead
  pids are swept at startup), validates the
  descriptor (sizes, ids, ranges, required functions) and copies all strings.
- **PluginHost** (control thread API; one loader `std::thread`): scans `plugins/` every 500 ms from the 30 Hz
  pump (signature = names/sizes/mtimes of `.dsp/.lib/plugin.json`, or the newest DLL); a change queues a job
  (queued jobs for the same plugin are coalesced; results of superseded jobs are dropped). Finished jobs are
  applied in `poll()`: registry entry `plugin:<name>` replaced, status event, `onModulesChanged`. Offline mode
  (`ks-render`, tests) compiles synchronously. A failed (re)compile keeps the previous good version registered.
  Deleting the plugin directory unregisters it.
- **Versions and lifetime.** Each successful load is a `PluginVersion` owning the `ModuleInfo` (registry points
  at it), the Faust factory or `PluginLibrary`. The registry factory lambda and every module hold
  `shared_ptr<const PluginVersion>`; graphs are deleted on the control thread, so the last release (instance
  `destroy`, then `deleteCDSPFactory`/`FreeLibrary` + temp copy deletion) happens there. Versions that never got
  registered (failed/superseded loads) may be released on the loader thread — no instance exists.
- **Hot reload.** registry entry replaced → `Session::refreshModules`: patch params re-normalized against the new
  specs (surviving ids keep their values, new ones default, removed dropped), DLL state blobs carried over via
  `saveState()` → `GraphBuilder` reuses a module only if its `ModuleInfo` *object* is still the registry's
  (`&module->info() == registry.find(type)`), so plugin modules are rebuilt and everything else is shared → normal
  swap (crossfade). Protocol: `list_plugins`, `reload_plugin`, `plugin_status` events (`docs/PROTOCOL.md`).
- **Isolation-lite.** Every call into plugin code (DLL functions, Faust compute) runs inside
  `platform::guardedCall` (SEH `__try`, `_resetstkoflw` on stack overflow) with MXCSR saved/restored (FTZ/DAZ
  re-asserted, §4.5). A fault mutes that instance for good (until a reload creates new ones) and is reported via
  an atomic counter on the version → `plugin_status` `faulted` + `log` error (polled on the control thread).
  NaN/Inf output zeroes the block and resets/clears the instance (a DLL instance is muted after 8 consecutive bad
  blocks); reports are edge-triggered, repeated at most every 5 s. This is not a sandbox: plugins
  are trusted local code (Faust `ffunction` can call C functions); only `plugins/` under the repo root is scanned.

## 11. Control protocol & security

JSON over WebSocket, see `docs/PROTOCOL.md` (source of truth for UI and engine; TS types mirror it).
- Bind 127.0.0.1 only. Reject WS handshakes whose `Origin` is not `http://localhost|127.0.0.1:{7341,5173}` or the
  static UI port (7340, served by `ix::HttpServer` from `ui/dist`). Handshakes without `Origin` (non-browser local
  clients) are accepted. WS paths other than `/` and `/ws` are rejected.
- Every path in a request is resolved and must lie inside allow-listed roots (repo `presets/`, `userdata/`,
  `assets/`, `plugins/`). Handlers validate input and reply `error` instead of crashing.

## 12. Testing & tooling (details: docs/TESTING.md)

- Unit (Catch2): DSP primitives, VoiceAllocator, ParamSet, PatchModel, GraphSwapper (incl. stress), preset JSON,
  protocol handlers.
- Render tests over all factory presets (chord + scale + sustain): no NaN/Inf, DC below threshold, peak ≤ 0 dBFS,
  not silent, tails decay. Tiers: fast = 48 kHz / 64; full = {44.1, 48, 96} × {32, 64, 512} before merge.
- `ks-render` CLI + `scripts/analyze_wav.py` (spectrum, pitch, envelope, spectrogram PNG): verify sounds without
  hardware.
- `ks-bench`: realtime factor per preset; require ≥ 2× headroom at 48 kHz/64 for the worst preset (a transition
  renders two graphs).
- UI: Vitest for store/protocol logic; visual checks in a browser against `keysynth-engine --no-audio`.

## 13. Extending — checklists

New instrument/effect: own dir; `Module` subclass + static `ModuleInfo`; one line in `BuiltinModules.cpp`; tests
`engine/tests/<module>_test.cpp`; ≥2 factory presets (instrument) or use in a preset (effect); row in §7.
New protocol message: `docs/PROTOCOL.md` + engine handler + `ui/src/protocol/types.ts` in the same change.

## 14. Licensing

Repo: AGPL-3.0-or-later (JUCE 8 open-source licence; bundled ASIO SDK GPLv3). `THIRD_PARTY_NOTICES.md` lists all
dependencies (msfa Apache-2.0, sfizz BSD-2, ixwebsocket BSD-3, nlohmann MIT, Catch2 BSL-1.0, sample libraries).
