# keysynth — Architecture

Source of truth for structure and invariants. If code and this doc disagree, fix one of them in the same change.
Revision 2 (after review #1: graph swap semantics, stable node ids, per-device MIDI queues, RT enforcement).

## 1. Goal and non-goals

Goal: MIDI keyboard (USB) → low-latency software synth → ASIO audio interface. Many built-in engines and presets,
user-extensible DSP (hot-reloaded plugins), web UI.

Non-goals (for now): hosting third-party VST3, DAW timeline, MIDI controller mapping, sidechain between layers (the
only external audio is the device input bus, §6 *Audio input*), MPE (API is ready for it: `noteId`), plugin builds of
keysynth itself.

Platforms: Windows 11 (ASIO first) and macOS 13+ on Apple Silicon (CoreAudio), same features and version (one
release, §16). Code stays portable: no Win32 / Cocoa calls outside `engine/src/platform/` (`*.mm` = Objective-C++,
macOS only) and the few `#if` blocks in `app/` (process-level glue).

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
- **Cloud threads** (§15): one CloudClient worker (API calls), one SyncService worker, two sample download
  workers. Network IO only; results are queued and drained by the message thread's 30 Hz pump. The relay host
  (§15 *Relay*) adds one RelayClient thread (connect / backoff / heartbeat) plus the socket's receive and send
  threads; relayed requests are posted to the message thread like local WS messages.
- **Desktop mode** (§16): the tray icon lives on the message thread; "open UI" probes mvv42.ru on a short-lived
  worker thread (one at a time) that only posts the browser launch back to the message thread.

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
  src/midi/<tool>/            One dir per MIDI processor (arp, chord, hold; §17).
  src/song/       Song model + JSON, `songs::` file store (factory + user setlists, §18).
  src/transport/  Transport clock, Metronome, DrumSequencer, (later) Looper.
  src/preset/     Patch data model <-> JSON, Migrations, PresetStore.
  src/control/    ControlServer (WS + static HTTP via ix::HttpServer), protocol handlers, snapshots.
  src/audio/      AudioHost (devices, ASIO, settings), MidiHub (all MIDI inputs).
  src/plugins/    PluginHost: Faust JIT (libfaust C API) + C ABI DLLs (sdk/include/keysynth/plugin_abi.h), hot reload.
  src/render/     OfflineRenderer (patch + notes/MIDI → buffer/WAV without a device).
  src/platform/   OS specifics (MMCSS "Pro Audio", power throttling, fault guard (SEH / signals), secrets at rest
                  (DPAPI / per-user key file), relay WebSocket (WinHTTP / NSURLSession), port owner, desktop
                  integration: single instance, autostart, Apple Events, browser launch — §16).
  src/cloud/      CloudClient (HTTPS to mvv42.ru, device token), SyncService (presets), CloudServices (§15).
  src/samples/    SampleCache + downloader (manifest, jobs, resume, verify), manifest path rules (§15).
  src/app/        main.cpp — wiring only; Tray (JUCE tray icon + menu), DesktopShell (log/console, UI opener),
                  keysynth.rc.in (version + icon resource). Two link variants, §16.
  tools/          ks-render (offline render), ks-bench (CPU per preset).
  tests/          Catch2 tests.
sdk/include/keysynth/plugin_abi.h   Stable C ABI for DSP plugins.
plugins/<name>/   Plugin sources (plugin.json + .dsp or .cpp). Built to plugins/.build/ (gitignored).
presets/factory/<category>/   Factory patches.   presets/patterns/  Drum patterns.
userdata/         (gitignored) user presets, settings.json, recordings.
assets/samples/   (gitignored) sample libraries; assets/samples.json manifest (committed).
ui/               Vite + React + TS.
scripts/          configure.ps1, fetch_samples.py, fetch_faust.ps1, build_plugin.ps1, analyze_wav.py,
                  build_installer.ps1, make_icon.py; macOS: configure.sh, fetch_faust.sh, build_plugin.sh,
                  build_app_macos.sh, start*.sh / stop-dev.sh.
installer/        keysynth.iss (Inno Setup 6, per-user installer), macos/Info.plist.in (keysynth.app), §16.
                  assets/icon/  app icon + macOS menu-bar template (generated).
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
enum class ModuleKind { Instrument, Effect, Midi };
class Module {
 public:
  virtual ~Module() = default;
  virtual const ModuleInfo& info() const = 0;        // typeId, displayName, kind, category, params, uiHints
  ParamSet& params();
  virtual void prepare(double sampleRate, int maxBlock) = 0;  // control thread, before going live
  virtual void reset() = 0;                          // kill voices/tails
  // Audio thread. Instrument: buffer cleared on entry. Effect: in-place stereo.
  virtual void process(AudioBlock& stereo, MidiEventSpan events, const ProcessContext& ctx) = 0;
  // Audio thread, kind Midi only (§17): events in -> events out, once per block before the layer's instrument.
  virtual void processMidi(MidiEventSpan in, MidiEventBuffer& out, const ProcessContext& ctx);
  virtual int tailSamples() const { return 0; }
  virtual int latencySamples() const { return 0; }
  virtual nlohmann::json saveState() const { return {}; } // non-param state (sample path, syx bank…)
  virtual void loadState(const nlohmann::json&) {}        // control thread, before prepare
  virtual bool loadStateWroteParams() const { return false; } // loadState wrote params (fm .syx voice)
  virtual std::string loadError() const { return {}; }    // control thread: failed resource load (bad sfz/syx)
  virtual std::optional<MissingResource> missingResource() const; // control thread: missing for a non-error reason
  virtual bool isReady() const { return true; }           // false while heavy resources load asynchronously
  virtual void setOfflineMode(bool) {}                    // control thread, after prepare: faster-than-RT render
  virtual void resourcesChanged() {}                      // control thread: a missing resource may exist now
};
```

- **Missing resources that arrive later** (`resourcesChanged`): the sample cache finished (or gave up on) a download
  (§15). Session calls it on every module of the latest graph; `sampler` instances waiting for that SFZ start their
  load (no rebuild, the module is not replaced).

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
  `missingResource` (`{ id, reason: "not_linked" | "offline", detail }`, sampler only: samples that can be downloaded
  but are not, §15) replaces the error with one friendly `log` warn notice per (reason, entry) per run; the detail
  (searched paths) only goes to stdout / engine.log.

- `ProcessContext`: sampleRate, numSamples, sampleTime, transport (tempo, ppq position, playing), and per-layer
  `ChannelState` {pitchBend (-1..1), modWheel, aftertouch, cc64 sustain, cc66 sostenuto, cc67 soft, expression}
  maintained by the core (engines don't parse CCs, but still receive raw events). The Engine keeps 17 global
  states (0 = omni merge, 1..16); each layer gets a copy for its zone channel with sustain/sostenuto masked when
  `zone.sustain` is false.
  `input` (`ExternalInput {left, right, channels}`): the current block's device input (§6 *Audio input*), read-only,
  `numSamples` valid samples; `channels` 0 = no input (null pointers), 1 = mono (`right == left`), 2 = stereo. The
  same view is passed to every module of the block (layers, master, both graphs of a transition).
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
 ├─ layers[] {nodeId, zone, midi[]{nodeId,type,params,state,bypass}, instrument{nodeId,type,params,state},
 │            fx[]{nodeId,type,params,state,bypass}}
 ├─ master {volume_db, fx[]}               ├─ LayerNode[] (zone filter, MIDI chain, instrument, fx, gain/pan, meters)
 ├─ rhythm {drums kit params, pattern}     ├─ RhythmNode (drums instance for the sequencer + metronome voice)
 └─ meta                                   └─ master fx → (+ metronome) → safety limiter
```

- Every layer and module slot has a stable `nodeId` (uint32, assigned by PatchModel, persisted in the patch).
  Params are addressed as `{nodeId, paramId}` — never by index.
- **PatchModel** (control thread) is the single source of truth. Protocol handlers only call PatchModel methods.
  A param write updates PatchModel and then the atomic in the latest *published* graph (lookup table
  nodeId → ParamSet*, rebuilt on publish). GraphBuilder reads params from PatchModel at publish time, so writes
  made during a build are never lost.
- Signal flow: MIDI (all inboxes merged per block) → song control keys removed (§18) → ChannelState update →
  Transport/DrumSequencer → each LayerNode zone filter (key/vel range on physical key, then transpose, channel) →
  layer MIDI chain (§17) → instrument → layer FX → sum → master FX →
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
  MIDI processors (§17) are shared like effects but produce no audio: a shared one is processed by the new graph
  only (the old graph passes its events through unchanged); they use no render-cache slot and do not affect whether
  downstream caches are valid. An unshared old one receives the AllNotesOff and stops.
- **Retire.** Old graph pointer → retire SPSC (capacity 8). If push fails, the audio thread keeps it and retries next
  block; it never frees. Control thread drains on a 50 ms timer and deletes.
- Device/sample-rate/buffer change: stop device → rebuild graph with new prepare args (no reuse) → restart
  (a restart with unchanged rate/buffer keeps the current graph; §6 *Device restarts behind our back*).
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
- **Device restarts behind our back.** The driver can change buffer size / sample rate at any time (ASIO
  `kAsioResetRequest` / `kAsioBufferSizeChange` / `kAsioResyncRequest`, e.g. a buffer change in the Yamaha Steinberg
  driver's own settings app). JUCE's ASIO device then closes and reopens itself 500 ms later on the message thread
  (`timerCallback`) and calls `audioDeviceAboutToStart` with the new size — **even when that reopen failed**, leaving a
  device that reports "playing" but never calls back. AudioHost therefore:
  - re-prepares on every `audioDeviceAboutToStart` with the device's *current* sample rate/buffer (only when they
    changed or no graph exists: unchanged restarts keep the graph); `Engine::process` splits any callback larger than
    `maxBlock` into ≤ maxBlock chunks, so a device delivering more than it announced is still safe (no allocation);
  - treats `audioDeviceAboutToStart` on a device that is not open / has `getLastError()`, `audioDeviceError` (note:
    JUCE's AudioDeviceManager currently swallows it — its `CallbackMaxSizeEnforcer` does not forward errors), a
    vanished device, and **no callbacks while it should run** (watchdog, message-thread timer every 100 ms, paused
    while the driver panel is open: > 500 ms = stall, logged; > 1.5 s = failure — longer than JUCE's own 500 ms
    reset + reopen, so a normal driver-initiated change is never interrupted) as a failure → **recovery**: close and
    reopen the same type/name with the driver's current settings (buffer: the user's explicit size if any — JUCE falls
    back to the driver's preferred size when it is not offered — else the preferred size; ASIO rate 0 → the driver's
    current rate), retry with backoff 1 → 2 → 4 … 30 s until callbacks flow again. A device that failed to open at
    startup is retried the same way;
  - persists `audio.bufferExplicit` next to `bufferSize`: an ASIO size is re-requested at startup only when the user
    chose it explicitly (`set_audio_device` / `--asio-buffer`, and the driver accepted it); after a driver-initiated
    change the driver's size wins, so a stale saved size is never forced;
  - logs every device event (start/stop/error/stall/recovery, with type, name, rate, buffer, latencies, reason) to
    `userdata/logs/audio.log` (timestamped, rotated at 1 MB, message thread only — the audio thread only bumps atomics
    and copies an error text into a fixed buffer) and forwards unexpected ones as `log { notify: true }`
    (`AudioControl::onLog`). `restart_audio` (protocol) runs the same recovery on demand.
- On the first callback `platform/` registers the audio thread with MMCSS (`AvSetMmThreadCharacteristicsW
  "Pro Audio"`) and disables power throttling for it (Intel hybrid P/E cores). macOS: nothing to do — CoreAudio's
  IO thread is already a real-time (time-constraint) thread. No ASIO there: the first start opens the system
  default device with a 128-sample buffer (CoreAudio would pick 512); later starts use the saved device.
- **Audio input** (external input bus; consumer: `vocoder`). Up to 2 input channels of the device are opened
  (`set_audio_input`, PROTOCOL.md *Audio input*); none by default, so the driver opens outputs only. Persisted as
  `audio.inputName` / `audio.inputChannels` (device channel indices, ascending) in `userdata/settings.json`. ASIO:
  the input device is the output device (`inputName` ignored); other device types name their input device. The
  selection is re-applied on every (re)open of the same device (startup, recovery, `restart_audio`, panel reopen);
  `set_audio_device` to a different device clears it. JUCE delivers the active input channels in ascending order;
  the callback passes them to `Engine::process(inputs, numIn, outputs, …)`, which hands each ≤ maxBlock chunk to
  the graph *without copying* as `ProcessContext::input` (channel 1 → left, channel 2 → right; one channel → both).
  Input and output of a callback are the same block, so the bus adds no latency beyond the device's own input
  latency. Offline renders and tests pass an optional `ExternalInput` to `Engine::processBlock` (default none). The
  Engine raises an input peak per block (`Telemetry::inputL/R` → `telemetry.meters.input`, dBFS) for the audio
  dialog's level meter.
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
| `arp`, `chord`, `hold` | MIDI processors (kind `midi`, §17): arpeggiator, one-finger chords / chord memory, hold/latch | live performance helpers |
| `drums` | Synth kit, 13 voices (kick, snare, clap, closed/open hat w/ choke, crash, ride, 3 toms, rim, cowbell, tambourine), each with model 808/909/Linn/Industrial (`kit` default or per-voice `<v>_model`) + level/tune/decay/tone/pan; GM key map, other octaves fold onto 36–47; keys-playable; used by the RhythmNode | 80s beats, Rammstein stomp |

Effects v1: `chorus` (Juno BBD I/II/I+II, custom, Dimension), `ensemble` (string-machine 3-phase), `phaser`
(4/6/8/12 stages), `flanger` (BBD, optional through-zero), `delay` (stereo/ping-pong/tape, tempo sync), `reverb`
(16-line FDN hall/plate/room/chamber/gated/shimmer), `drive` (tube/fuzz/tape, IIR-oversampled 2x/4x), `rotary`
(Leslie 122/147 horn+drum, Doppler/AM, ramped slow/fast/brake, mod-wheel speed, ReadOnly `horn_rpm`/`drum_rpm`),
`tremolo` (amp trem with L/R phase / equal-power autopan; sine/tri/smoothed square; free or tempo-synced),
`compressor` (VCA/opto, `gr_db` meter), `eq` (HP/LS/3 bells/HS/LP), `limiter` (master safety, fixed).
Effects v2: `vocoder` (channel vocoder, 8/16/20/32 bands: modulator = the device input bus (§6) or an internal
vowel/noise source; carrier = the layer's audio; 4th-order band-pass pairs (two `dsp/Svf` stages), envelope
followers with attack/release, formant shift of the carrier bands, sibilance noise for unvoiced consonants, carrier
mix, input gain + gate, input L/R/mono), `wah` (inductor wah, Cry Baby / Vox curves: resonant band-pass + low-pass
body, pedal from the param, mod wheel or CC11 expression; auto-wah envelope; LFO with `sync`), `echo` (Binson
Echorec magnetic drum: one record head, 4 equally spaced playback heads, `heads` switch over 12 combinations, swell =
regeneration from the selected heads, tone, wow/flutter of the drum speed, valve saturation, `time` = head spacing or
`sync`), `tapestop` (tape stop / start: varispeed read of a circular buffer, speed 1 → 0 → 1 with a curve; triggered
by the `stop` param, a MIDI note or a CC; a start from standstill restarts the read head at the write head and
spins it up, the delay accumulated while slower than 1 is removed by a 10 ms crossfade to the dry path once the
speed is back at 1 — while running the output is the dry input, zero latency).
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
  at runtime from `<KS_FAUST_DIR>/lib/faust.dll` (macOS: `lib/libfaust.2.dylib`, from the official arm64 dmg via
  `scripts/fetch_faust.sh`) (function table, no import lib): faust.dll uses the dynamic CRT, keysynth the static
  one, so no C++ objects may cross (kept on macOS too). Factory create/read/write/delete hold one mutex (a factory released
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
  `plugins/.build/<name>-<hash>.dll` (temp name + rename); on macOS `scripts/build_plugin.sh <name>` (clang++ -O2,
  `-fvisibility=hidden`) into `<name>-<hash>.dylib` (`platform::kSharedLibraryExt`). The host loads the newest
  `<name>-*.dll` / `.dylib` through a copy `<temp>/keysynth-plugins/<pid>/<name>-<hash>-<n>.<ext>` (so builds can
  overwrite/delete; only directories of dead pids are swept at startup), validates the
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
  `platform::guardedCall` (Windows: SEH `__try`, `_resetstkoflw` on stack overflow; POSIX: SIGSEGV/SIGBUS/SIGILL/
  SIGFPE handlers + `sigsetjmp`/`siglongjmp`, re-installed when another component replaced them, an alternate
  signal stack per thread from a static pool, C++ exceptions caught; codes reported as the Windows ones, e.g.
  `0xC0000005`, so messages match) with the FP control register (MXCSR / ARM64 FPCR) saved/restored (FTZ/DAZ
  re-asserted, §4.5). A fault mutes that instance for good (until a reload creates new ones) and is reported via
  an atomic counter on the version → `plugin_status` `faulted` + `log` error (polled on the control thread).
  NaN/Inf output zeroes the block and resets/clears the instance (a DLL instance is muted after 8 consecutive bad
  blocks); reports are edge-triggered, repeated at most every 5 s. This is not a sandbox: plugins
  are trusted local code (Faust `ffunction` can call C functions); only `plugins/` under the repo root is scanned.

## 11. Control protocol & security

JSON over WebSocket, see `docs/PROTOCOL.md` (source of truth for UI and engine; TS types mirror it).
- Bind 127.0.0.1 only. `Host` must be `127.0.0.1:<port>` / `localhost:<port>` (DNS rebinding) on WS and HTTP.
  `Origin` classifies the session as `local` (absent, the local UI ports, `:5173` in dev builds only) or `web`
  (`https://mvv42.ru`); anything else (incl. `null`) is refused. WS paths other than `/` and `/ws` are rejected.
  ixwebsocket has no pre-upgrade hook, so a refused WS handshake is closed with 4003 right after the upgrade and
  never reaches the protocol handler. Rules and the `web` allow-list: PROTOCOL.md *Sessions, Origin, Host*
  (`control/Trust.h`, checked in `ProtocolHandler` before dispatch).
- **Port conflicts.** ixwebsocket sets `SO_REUSEADDR`, which on Windows lets a second socket bind a port that is
  already listening. So before each bind `ControlServer::probe` tries an exclusive bind (`SO_EXCLUSIVEADDRUSE`) of
  exactly `127.0.0.1:<port>`; when refused, the owner is named from the listener table (`platform/PortOwner`:
  `GetExtendedTcpTable` IPv4 + IPv6 → pid → image; macOS: `libproc`, the user's own processes; a listener on
  `127.0.0.1`, `0.0.0.0` or `[::]`) and classified (`control/PortConflict`: `keysynth` = image
  `keysynth-engine[.exe]`, else `foreign`). On macOS (BSD sockets) a busy port is refused by the bind itself; the
  trial bind (no `SO_REUSEADDR`) only names the owner earlier. Refused without a nameable
  listener (TIME_WAIT connections of a previous run) → the real bind decides. A v6-only `[::]` / `[::1]` listener
  and a non-exclusive `0.0.0.0` one (the more specific 127.0.0.1 bind wins loopback traffic) do not block; `[::1]`
  is logged (`localhost` clients may reach it). WS (7341) and HTTP (7340) bind independently.
  Headless runs exit 1 with the classified message; desktop mode handles it without a modal box (§16). The WS port
  is probed before audio/plugins/cloud start, bound after them (a hello must not wait for a slow device open).
  `retryListen()` binds whatever is still missing.
- Every path in a request is resolved and must lie inside allow-listed roots (app `presets/`, the profile root
  `userdata/`, `assets/`, the plugins dir, the sample cache; AppPaths). Handlers validate input and reply `error`
  instead of crashing.

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

Repo: AGPL-3.0-or-later (JUCE 8 open-source licence; bundled ASIO SDK GPLv3).
`THIRD_PARTY_NOTICES.md` lists all dependencies (msfa Apache-2.0, sfizz BSD-2, ixwebsocket BSD-3, nlohmann MIT,
Catch2 BSL-1.0, sample libraries).

## 15. Cloud, sample cache, sync

Design `docs/specs/2026-10-12-cloud.md` (§11 wins), REST + storage contract `docs/specs/2026-10-12-cloud-api.md`,
wire messages PROTOCOL.md *Cloud & samples*. Nothing here runs on the audio thread.

- **AppPaths** (`core/AppPaths`): *installed* mode iff `installed.flag` is next to the exe (app dir = exe dir, data
  root `%LOCALAPPDATA%\keysynth`) or in `keysynth.app/Contents/Resources` (app dir = Resources, data root
  `~/Library/Application Support/keysynth`), else *dev* (repo checkout, data root `<repo>/userdata`, as before); `KS_DATA_DIR`
  replaces the data root (`discover()` only). Holds every location of the contract table (settings, `cloud.json`,
  logs, plugins + build/Faust cache, sample cache) and the **active profile** (`userdata`: local profile or
  `accounts/<id>`). Wire paths are *logical* (`userdata/…` = active profile, `presets/…` = app dir,
  `assets/samples/…` = asset dirs or cache); `resolveAllowed` maps logical → disk, `relativeToRoot` disk → logical.
- **HTTP** behind `cloud::HttpTransport` (one blocking `send()` with streaming sink + cancel flag); production =
  `JuceHttpTransport` (JUCE `WebInputStream`, WinINet on Windows, NSURLSession on macOS, no redirects followed), tests = fakes. Base URL
  compiled in (service root `https://mvv42.ru/keysynth`; requests go to `<root>/api/...`, nginx maps `/keysynth/api/` → app `/api/`), dev override `--cloud-api` / `KS_CLOUD_API` (dev builds only).
- **CloudClient** (`cloud/`): `cloud.json` (deviceId, link; the token protected via `platform/Dpapi`: DPAPI on
  Windows; macOS: AES-256-CBC + HMAC-SHA256 with a random per-user key file
  `~/Library/Application Support/keysynth/.keys/<entropy>.key` (0600) — same blob-at-rest contract and the same
  "only this OS user" guarantee as DPAPI; not the keychain, whose items trust the creating binary's signature and
  would prompt after every ad-hoc-signed update), link /
  unlink / status, bearer header on every call, drops the token on any 401. One worker thread for its own calls;
  `api()` is callable from other workers (thread-safe, token under a mutex).
- **SampleService** (`samples/`): manifest (cached `manifest.json` + ETag), `cache.json` index, jobs (entry /
  library / all, FIFO, auto-fetch at the front), 2 download workers, `.part` + `Range`/`If-Range` resume, SHA-256
  verify (`core/Sha256`), atomic rename, strict manifest path validation (`samples/SamplePathRules`) before any
  write, refcounted delete. Progress/done/error are queued and drained by the pump (≤ 5 Hz progress).
  The sampler's path resolution (`instruments/sampler/SamplePaths`) also searches the cache root; a missing
  `assets/samples/<entry>` asks the registered fetch hook (SampleService) and waits with `loading` = 1; job end →
  `Session::notifyResourcesChanged()` → `Module::resourcesChanged()`.
- **SyncService** (`cloud/`): presets and songs (§18: same algorithm per collection, `/api/presets` then
  `/api/songs` in one pass, one `sync.json` with a section per collection), engine is the only writer; algorithm of the REST contract §3.4 on its
  own worker (link, 60 s, local change, `sync_now`); local file steps under `Session::profileMutex()` (also held
  by preset saves on the message thread), never across network calls.
- **CloudServices** (`cloud/`) owns the three, implements the cloud protocol messages (async replies through
  `poll()`), switches Session profiles on link/unlink, and is optional (tests and `ks-render` run without it).
- **Relay host** (remote control from a browser anywhere, spec `docs/specs/2026-10-12-remote.md`, §10 wins). The
  engine keeps no listening port for it: while linked, `cloud::RelayClient` (owned by CloudServices, started by the
  app after the ControlServer exists) holds one outbound WebSocket `<api root>/api/relay?role=host` (`https`→`wss`)
  with `Authorization: Bearer <device token>`.
  - Socket: `platform::makeDefaultWebSocket()` behind the abstract `platform::WebSocket` (tests use a fake).
    Windows `platform/WinHttpWebSocket`: WinHTTP with `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`, TLS for `wss`, custom
    headers, blocking handshake, a receive thread (callbacks) and a send thread draining a queue (`send()` never
    blocks the caller). macOS `platform/WebSocket_mac.mm`: `NSURLSessionWebSocketTask` (system proxy, TLS), the
    same contract with callbacks on the session's serial delegate queue. No new dependency.
  - RelayClient thread: connect while linked (token re-read each attempt; a changed token reconnects), `hello
    {deviceId,name,version}` after the upgrade, app-level `{t:"ping"}` every 20 s, `{t:"pong"}` to server pings,
    close + reconnect after 45 s without any frame. Failures back off 1 s → 30 s (doubling, ±20 % jitter; reset by
    a connection that stayed up ≥ 10 s, so a flapping server is not hammered). 401/403 at the upgrade = CloudClient's 401 path (`clearLinkIfToken`, `cloud_status`
    `linked:false`); it then idles until linked again (CloudServices wakes it on every status change).
  - Inbound frames are parsed on the receive thread and posted to the message thread: `{t:"controller",cid,joined}`,
    `{t:"msg",from,msg}` (an unknown `from` joins implicitly), `{t:"pause"|"resume",cid}`, `{t:"controllers",count}`
    (0 → all leave); a disconnect removes every remote client.
  - **Virtual clients** `r:<cid>` live in `ControlServer::clients_` (no socket) with `Trust::Remote` =
    `webRestriction` + deny `cloud_link`, `cloud_unlink`, `set_audio_device`, `set_audio_input`, `open_audio_panel`, `samples_delete`,
    `reload_plugin` (`control/Trust`, default-deny; `hello` replies `trust:"remote"`). Their requests run through
    the same ProtocolHandler.
  - Outbound (`control/RemoteRouter`, message thread, owned by ControlServer): `Reply` → `{t:"msg",to:cid}`;
    `Broadcast` → `to:"*"` once; `Others` → `to:"*"` plus `except:<cid>` when the origin is remote. `lossy:true` on
    `telemetry`, `midi`, `samples_progress` (the server may drop only those). Remote rates: telemetry ≤ 10 Hz,
    midi notes merged and flushed ≤ 15 Hz, `param` coalesced per (node,param) ≤ 20 Hz (latest value wins; the pump
    calls `ControlServer::tickRemote()` to flush). Telemetry/midi go nowhere when no remote controller is active;
    paused controllers (hidden tab) are skipped (fan-out per cid instead of `*`). Pending params are flushed before any
    later non-lossy event (a `state` to everyone supersedes them). Lossy frames are dropped when the socket queue
    holds > 256 KB; a non-lossy frame is never dropped silently: a reply > 1 MB becomes `error too_large` to its cid,
    any other undeliverable frame (> 1 MB broadcast, 16 MB queue full) closes the socket → reconnect → controllers
    re-`hello`. Late frames of a retired socket are ignored (generation counter), so no ghost clients remain.
- **Web UI** (`ui/src/platform/`, `ui/src/store/cloudStore.ts`, `ui/src/components/cloud/`): one bundle, two
  modes picked at runtime (`platform/mode.ts`): *platform* (served from `https://mvv42.ru/keysynth/`, build
  `npm run build:platform`, base `/keysynth/`) and *local* (served by the engine at :7340 or the dev server, base
  `/`). Platform mode uses only the framework-free `@mvv42/kit/platform` entry (`useMe`, `apiJson`,
  `requestLogin`, `logout`, shell `READY`) — not `Mvv42Provider`/Mantine, whose global CSS would restyle the synth
  UI; the account button is our own. Engine discovery (`platform/discovery.ts`): no socket before the engine was
  seen (localStorage `keysynth.engineSeen`) or the LNA permission is granted; phases never-seen / connecting /
  permission-denied / not-running / connected; until the first `state` the page shows the download card
  (`EngineGate`). Account link (`platform/link.ts` + cloud store): plan from (`/me`, `hello.cloud`, owner check via
  `GET /api/devices`), mint `POST /api/devices` → `cloud_link`; re-linking to another account only after the
  user's confirmation; an explicit unlink holds auto-linking (localStorage `keysynth.linkHold`). The device token
  exists only in local variables of the link call (never stored, logged or put in a URL). Update check: GitHub
  `releases/latest` vs `hello.version`.

## 16. Desktop app (Windows and macOS packaging)

Design `docs/specs/2026-10-12-cloud.md` §7 + §11 (Windows packaging); build/test `docs/TESTING.md` *Release /
installer*. The engine stays one process; desktop mode only adds a tray icon and process-level glue.

- **Targets.** `keysynth-engine` (console subsystem: dev, scripts, CI, `build/bin/<Config>/`) and `keysynth-app`
  (windows subsystem, same sources, `build/bin/<Config>/app/keysynth-engine.exe`, shipped by the installer). Both
  carry the version resource (`KS_VERSION`: CMake `project()` version, release CI passes the tag) and the icon
  (`assets/icon/keysynth.ico`, generated by `scripts/make_icon.py`; the tray PNG is embedded at configure time).
  `keysynth-app` has no console: it attaches to the parent's console when started from one, else writes
  stdout/stderr to `<data>/logs/engine.log` (previous run: `engine.prev.log`); fatal start errors show a message box.
- **Desktop mode** = installed mode (AppPaths, `installed.flag`) or `--tray` / `keysynth://` launch, unless
  `--no-tray`. It adds: tray icon (`app/Tray`, JUCE `SystemTrayIconComponent`; left click = open UI, right click =
  menu: open keysynth, open locally, restart audio, autostart toggle, quit; tooltip: version, device, rate/buffer,
  cloud link; Russian when the Windows display language is Russian) and single instance
  (`platform/DesktopIntegration`): named mutex `keysynth-engine` (= Inno `AppMutex`) + auto-reset events
  `keysynth-engine-open` / `-quit` polled by the tray at 10 Hz. A second launch signals `open` (plain launch) or
  nothing (`--tray`, `keysynth://`) and exits; `--quit` signals `quit` (scripts; the installer sets the
  event itself from `[Code]`). A protocol argument makes main ignore every other argument (a shell-split URL must
  not inject flags). Headless runs (dev default, `--no-tray`) are unchanged: no mutex, no tray.
- **Open UI**: GET `https://mvv42.ru/keysynth/health` (3 s, no redirects) on a worker; 200 → web UI
  `https://mvv42.ru/keysynth/`, else the local UI `http://127.0.0.1:<http-port>/` when the static server runs.
  Plain launch opens it at start; `--tray` (autostart) and `keysynth://` (the web page started us) do not.
- **Port busy at start** (§11 *Port conflicts*): owner `keysynth` (another engine outside our single instance,
  e.g. a headless dev build) → open the UI (it talks to that engine; not for `--tray` / `keysynth://`) and exit 0.
  `foreign` → keep running in the tray: balloon + tooltip «Порт 7341 занят процессом <name> (PID n). Закройте его —
  keysynth переподключится автоматически.» (English when the display language is not Russian), retry every 3 s,
  balloon again once bound. Until the WS port is bound the UI is not opened (a click on the tray repeats the
  balloon; a pending open-on-start runs after binding), so a browser never lands on the foreign server. HTTP-port
  conflicts only disable «Открыть локально» until bound (tooltip + log).
- **Autostart**: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `keysynth` =
  `"<exe>" --tray`, written by the tray toggle or the installer task (same value).
- **Faust** in the install: `faust.dll` + `share/faust/*.lib` next to the exe are preferred over the compile-time
  `KS_FAUST_DIR` (after `$KS_FAUST_DIR`); faust.dll's dynamic CRT (`msvcp140`, `vcruntime140`, `vcruntime140_1`)
  ships app-local. The engine itself links the static CRT.
- **Installer** (`installer/keysynth.iss`): per-user `%LOCALAPPDATA%\Programs\keysynth`, Start-menu entry
  `keysynth` (no folder, pinnable), optional desktop icon + autostart, `keysynth://` under `HKCU\Software\Classes`,
  Faust example plugins into `<data>/plugins` (never overwritten), fixed install dir. A running engine: interactive
  setup/uninstall show Inno's AppMutex prompt (nothing is killed by merely opening the wizard), then the quit event
  is set in `PrepareToInstall` / `usUninstall`; silent setup stops it at start and restarts it with `--tray`
  afterwards. Keeps `%LOCALAPPDATA%\keysynth` unless the user agrees (silent: `/DELETEDATA`). English +
  Russian (chosen from the Windows UI language).
- **macOS** (same features, differences only): `keysynth.app` built by `scripts/build_app_macos.sh` (the
  `keysynth-app` binary as `Contents/MacOS/keysynth-engine`, `Info.plist` from `installer/macos/Info.plist.in`:
  `LSUIElement` = menu-bar app without a Dock icon, `keysynth://` scheme, microphone usage text for the audio
  input; `Contents/Resources` = the Windows app dir: `installed.flag`, presets, `ui/dist`, `lib/libfaust.2.dylib`
  + `share/faust/*.lib`, Faust example plugins), ad-hoc signed (no Apple Developer ID: the first start of a
  downloaded copy needs «Open Anyway»; no hardened runtime, so the Faust JIT and user `.dylib` plugins load), in
  `keysynth-macos[-X.Y.Z].dmg`. Differences in the glue:
  - `main()` creates `NSApp` before JUCE's initialiser (`platform::initApplication`: JUCE does it only for
    `START_JUCE_APPLICATION` apps; without it `[NSApp run]` returns at once) and sets the accessory policy in
    desktop mode.
  - LaunchServices never starts a second process for a running app; it sends Apple Events
    (`platform::installAppEventHandlers`, after JUCE's, replacing its URL handler): *reopen* (Finder / Dock /
    Spotlight launch) = an open request, *quit* (logout, shutdown) = a clean quit (AppKit would `exit()`),
    `kAEGetURL` = a `keysynth://` launch — it arrives after the run loop starts, so open-on-start waits 500 ms
    and is skipped for a link launch.
  - Single instance (`--quit`, dev `--tray` runs): exclusive `flock` on `$TMPDIR/keysynth-engine-<uid>.lock` + a
    datagram socket next to it carrying open/quit (polled like the Windows events).
  - Tray = own `NSStatusItem` (`platform/MenuBar`, AppKit; JUCE's macOS tray has no working menu, tooltip or
    balloons): template icon `assets/icon/keysynth-tray-template.png` (tinted orange while a notice is pending).
    Left click = status popover: engine state pill (running / port busy / no audio), notice banner (port conflict,
    failed audio restart — the Windows balloons), audio device + rate/buffer/latency, CPU and output meters (Pump
    keeps decayed peaks of the telemetry it already polls), voices, preset, MIDI inputs, UI / remote clients, cloud
    account; buttons open keysynth / locally, restart audio, «At login» switch, log (Finder), quit. Refreshed at
    4 Hz only while open; data via `TrayActions::status` (a `MenuBarStatus` snapshot built in main.cpp), actions are
    the same callbacks as on Windows. Right click / control-click = native menu with the same actions.
    «Open at Login» = `SMAppService.mainAppService` (no-op outside a bundle).
  - No console: started without a terminal, stdout/stderr go to `<data>/logs/engine.log`; fatal start errors use a
    system alert (`CFUserNotificationDisplayAlert`).
  - No installer: the first start of the bundle copies the example plugins into `<data>/plugins` (never over an
    existing dir); uninstall = drag to the Trash (the data dir stays).
- **Release**: `.github/workflows/release.yml` builds and tests both platforms; the GitHub Release (tag `vX.Y.Z`)
  is published only when both succeed, so Windows and macOS always ship the same version. The web UI picks the
  download by OS (`ui/src/platform/os.ts`).

## 17. MIDI processors (per-layer MIDI tools)

Live-performance helpers that transform a layer's MIDI before its instrument: arpeggiator, one-finger chords /
chord memory, hold/latch. They are ordinary modules (`ModuleKind::Midi`, catalog kind `"midi"`, category `MIDI`,
dirs `engine/src/midi/<tool>/`, one line each in `BuiltinModules.cpp`), so node ids, ParamSet, `set_param`, presets,
relay and the generic UI work unchanged.

- **Patch**: `layer.midi[]` = ordered `ModuleSlot`s (with `bypass`), ≤ 8 per layer (`PatchModel::kMaxMidiPerChain`);
  JSON key `"midi"` (PRESETS.md), optional, written only when non-empty — no format bump (older engines ignore it).
  Protocol: `add_fx` with a `midi`-kind module adds to the layer's MIDI chain (refused on the master chain);
  `remove_fx` / `move_fx` (within the MIDI chain) / `set_fx_bypass` work by node id (PROTOCOL.md).
- **Processing** (audio thread, `RackGraph::render`): zone filter → for each non-bypassed MIDI slot in order
  `processMidi(in, out, ctx)` (ping-pong between two preallocated `kMaxEventsPerBlock` buffers of the LayerNode;
  `MidiEventBuffer::push` drops events when full) → the core sorts the output stably by `sampleOffset` (insertion
  sort, no allocation; a processor may append in any order) → instrument and layer FX get the final events. `ctx`
  carries the layer's ChannelState, `sampleTime` (absolute block start) and the transport (tempo, ppq at block
  start, playing). Output offsets stay inside `[0, numSamples)`. A processor releases every note it started when it
  receives `AllNotesOff`/`AllSoundOff` (transitions, panic) and passes that event on; a note-off for a note it never
  took (played before it was inserted) is passed on unchanged. All state lives in fixed arrays (§4; ≤ 16 held
  input notes per tool).
- **Releasing notes on edits**: bypassing a processor (`set_fx_bypass`, no rebuild) runs it once with an
  AllNotesOff input; its note-offs go to the instrument, the AllNotesOff itself is dropped (other notes keep
  sounding). A rebuild that changes a layer's MIDI chain (processor added, removed or recreated: module pointers
  differ from the outgoing graph's, `GraphSwapper::begin` → `LayerNode::flushNotes`) sends one AllNotesOff to that
  layer in the new graph, since a removed processor can no longer release what it started.
- **`arp`** — `mode` Up / Down / Up-Down / As played / Random / Chord, `octaves` 1–4, `sync` (the shared
  NoteDivision enum, §7; Off → free `rate` in Hz), `gate` (0.05–1 of a step; ≥ 0.999 = legato: the off lands on
  the next step's sample, before its note-on), `swing` (every 2nd step later by `swing × ½ step`, as in the drum
  sequencer), `latch`, `vel_mode` As played / Fixed (`velocity`) / Accent (every `accent_every`-th step from the
  pattern start at 127, the others at `velocity`), `retrigger` (a new chord restarts the pattern and the own clock).
  - Clock: **transport playing and `sync` ≠ Off** → steps on the transport grid: step k at ppq `k × d` (+ swing on
    odd k), fired at the first sample ≥ its exact position (ε = 1e-4 samples, the DrumSequencer rule), computed from
    the block-start ppq and tempo; after a key press the first step is the next grid point at or after it.
    **Otherwise own clock**: step 0 at the key press that starts the arp, then every `L` samples (`L` = division at
    the transport tempo, or `sampleRate / rate` when sync is Off) in absolute samples (`ctx.sampleTime`), so timing
    is identical for any block size; a change of `L` (tempo, rate, sync) re-anchors at the last fired step. A switch
    between the clocks (transport start/stop) re-anchors at the next step.
  - Input notes at the same sample as a step are part of that step. Held notes (≤ 16) are ordered by pitch
    (Up/Down/Up-Down, repeated per octave) or by press order (As played); Up-Down does not repeat the turning notes;
    Random never repeats the previous index when there is a choice (xorshift seeded at prepare → deterministic);
    Chord plays all held notes on every step (octave = step mod `octaves`).
  - Latch: releasing keys keeps the arp running; the next key press after all keys were up replaces the chord;
    turning latch off with no key down stops it. When the arp stops, the sounding step note ends at its gate time.
  - CC, pitch bend and pressure pass through; the sustain pedal reaches the instrument (arp notes ring like played
    ones).
- **`chord`** — every input note plays a chord: `type` Major … Min9 (fixed interval tables) or Memory (learned
  intervals in hidden params `mem_count`, `mem_1`…`mem_8`, semitones above the lowest learned note), `voicing`
  Close / Open (every 2nd tone +12) / Wide (Open + root −12), `inversion` 0–3 (the lowest k tones +12). Output tones
  are reference-counted (two keys sharing a tone: one note-on; the off when the last of them is released).
  ReadOnly hidden readouts `held_count`, `held_1`…`held_8` (current input notes, lowest first) let the control
  thread learn: protocol `chord_learn { node }` writes the held notes (≥ 2) into `mem_*` and `type` = Memory through
  `Session::setParam` (patch, dirty flag, `param` events). Only `Session::learnChord` knows these param ids.
- **`hold`** — `hold` on: note-offs are deferred (sustain-like for any instrument; in front of an `arp` it acts as
  its latch); off releases everything held. `mode` Sustain (a re-struck held key sends off + on) or Latch (the first
  key after all keys were up releases the previously held notes: chord replace). `pedal`: CC64 drives `hold` and is
  consumed (not passed on).
- Tests `[midi]` (engine/tests/test_midi.cpp): arp step times identical for block sizes 32/64/333 (own clock and
  transport grid), modes, octaves, gate/legato, swing, latch, retrigger, velocity modes; chord tables, voicings,
  memory learn; hold modes; AllNotesOff; `rt::violationCount() == 0` while rendering through the Engine.

## 18. Songs (setlists)

A song = metadata + ordered sections; selecting a section loads its preset and sets tempo / drums. Files:
`presets/songs/<slug>.json` (factory, read-only, shipped by the installer) and `userdata/songs/<slug>.json` (active
profile, synced to the account like presets). Schema PRESETS.md *Songs*; wire messages PROTOCOL.md *Songs*.

- `song/Song` (data + lenient JSON + validation; unknown/reserved keys are kept verbatim and written back; out-of-range
  tempo / meter in a file are dropped with a warning) and `songs::` in the same file (list / read / save / delete on
  the control thread; writes hold `Session::profileMutex()` and then call `onUserPresetsChanged` → sync). The active
  song is re-read from disk on every section switch, so a version pulled by sync applies at the next switch.
- **Session** owns the active song (logical path, parsed song, section index) and the global control keys
  (`settings.json` key `songs.control_keys`, read-modify-write like AudioHost). `selectSection(i)` applies, in order:
  `preset` (`loadPreset`; a bare slug = `userdata/presets/<slug>.json`) → song `time_sig` → section `pattern` (with
  its own meta) → tempo (section, else song) → `drums` (true: drums on + transport playing, the position keeps
  running when already playing; false: transport stops; absent: unchanged). A failing step is reported (`log` error,
  `notify`) and the remaining steps still apply.
- **Control keys** (section switching from the keyboard): effective = the song's `control_keys` if present, else
  the global ones; active only while a song is selected. The Engine holds the two note numbers as atomics (−1 =
  none); `gatherEvents` drops NoteOn events of those keys (any channel; their NoteOffs pass, harmless) before
  ChannelState, telemetry and layers, and bumps a press counter per key (atomics, never lost). The 30 Hz pump
  (`Session::pollSongKeys`) turns counter changes into next / prev and broadcasts `song_state` + `state`. The sustain
  pedal and all other keys are untouched.
- **Sync**: SyncService runs the cloud-api §3.4 algorithm for `userdata/songs/` against `/api/songs` (body key
  `song`) right after presets in each pass; `sync.json` keeps `songs` next to `presets`; conflicts become
  `<slug>.conflict-<ts>.json` in `userdata/songs/`. The first link copies local songs into the account profile like
  presets and patterns.
- Reserved, not implemented (schema documented in PRESETS.md): notation (MusicXML) per song / section, backing
  tracks, bar counts / auto-advance; with them a lossy `position` event (remote spec §10.16).
