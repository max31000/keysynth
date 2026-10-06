# Engine ⇄ UI protocol

WebSocket `ws://127.0.0.1:7341`, text frames, one JSON object per frame.
Envelope: `{ "type": string, "id"?: number, ... }`. Requests may carry `id`; the engine's reply has the same `id`
and `type` `"<request>_ok"` or `"error"` (`{ "type":"error", "id":?, "code":string, "message":string }`).
Engine never crashes on bad input. UI types live in `ui/src/protocol/types.ts` — keep them in sync with this file.

Addressing: every layer and module slot has a stable `node` id (uint32, in the patch JSON as `node`).
Params are addressed `{ node, param }`. Structural ops use `layer` node ids and FX positions. Transport is not a
node; it has its own `transport` message.
Security: engine binds 127.0.0.1 and rejects foreign `Origin`s; paths must be inside allow-listed roots
(ARCHITECTURE §11).

## Client → engine

| type | payload | reply |
|---|---|---|
| `hello` | `{ client: "ui", version }` | `state` (full snapshot) |
| `get_catalog` | — | `catalog_ok { modules: ModuleInfo[] }` (typeId, name, kind, category, params: ParamSpec[], uiHints) |
| `set_param` | `{ node, param, value }` | none (fire-and-forget; engine echoes via `param` event to other clients) |
| `load_preset` | `{ path }` | `load_preset_ok`, then `state` |
| `save_preset` | `{ name, category?, overwrite? }` (current patch → userdata) | `save_preset_ok { path }` |
| `list_presets` | — | `list_presets_ok { presets: { path, name, category, tags, factory }[] }` |
| `get_patch` / `set_patch` | `{ patch }` (full patch JSON, see PRESETS.md) | `state` |
| `add_layer` / `remove_layer` | `{ index? }` / `{ layer }` (layer node id) | `state` |
| `set_zone` | `{ layer, zone: partial Zone }` | none (param-like, no rebuild) |
| `set_instrument` | `{ layer, module }` (module typeId) | `state` |
| `add_fx` / `remove_fx` / `move_fx` | `{ layer, module, index? }` / `{ node }` / `{ node, to }` (`layer: 0` = master chain) | `state` |
| `set_fx_bypass` | `{ node, bypass }` | none |
| `rescan_midi` | — | `devices` |
| `note` | `{ on: bool, note, velocity, channel? }` (virtual keyboard) | none |
| `cc` | `{ cc, value, channel? }` | none |
| `transport` | `{ playing?, tempo?, metronome?, metronome_volume?, pattern?, drums?, drums_volume?, swing?, time_sig?, count_in? }` (see *Rhythm*) | none (`error` on a bad pattern path) |
| `list_patterns` | — | `list_patterns_ok { patterns: { path, name, time_sig, bars, tempo, factory }[] }` |
| `get_pattern` | `{ path? }` (default: the current pattern) | `get_pattern_ok { path, edited, pattern: Pattern }` |
| `set_pattern` | `{ pattern: Pattern }` (replaces the current pattern; live, sample-accurate swap) | `set_pattern_ok`; others get a `pattern` event |
| `save_pattern` | `{ name, overwrite? }` (current pattern → `userdata/patterns/<slug>.json`) | `save_pattern_ok { path }`, then `state` to all |
| `list_devices` | — | `devices` |
| `set_audio_device` | `{ device_type, name, sample_rate?, buffer_size? }` | `devices` (also to others; + `log` warn if the driver kept another buffer size), then `state` to all; `error` `busy` while the driver panel is open |
| `open_audio_panel` | — (opens the driver's own settings panel, ASIO) | `open_audio_panel_ok`, or `error` `not_available` / `busy` (already open) (see *Audio latency*) |
| `restart_audio` | — (close + reopen the current device with the driver's current buffer/rate; ASIO: the panel's size) | `devices` (also to others), then `state` to all; `error` `busy` while the driver panel is open, `device_error` when the reopen failed (the watchdog keeps retrying), `not_available` without an audio host |
| `panic` | — | none (all notes off, reset tails) |
| `list_plugins` | — | `list_plugins_ok { plugins: PluginStatus[], faust: { available, version, reason } }` |
| `reload_plugin` | `{ name }` (plugin directory name, `[a-z][a-z0-9_]*`) | `reload_plugin_ok { name }`, then `plugin_status` events |

## Engine → client (events)

| type | payload |
|---|---|
| `state` | `{ patch, presetPath, dirty, audio: AudioStatus, transport }` — full snapshot |
| `param` | `{ node, param, value }` — another client changed a param |
| `telemetry` | `{ cpu, xruns, voices, meters: { master:[l,r], layers:{ "<node>":[l,r] } }, readouts: { "<node>": { param: value } } }` ≤30 Hz, peaks in dBFS |
| `midi` | `{ notes: [ {note, on, velocity} ] }` batched ≤30 Hz, for keyboard highlighting |
| `pattern` | `{ path, edited, pattern: Pattern }` — another client changed the current pattern (`set_pattern`) |
| `devices` | `{ types: string[], current: AudioStatus, available: { type, names[], sampleRates: number[], bufferSizes: number[] }[], midiInputs: string[] }` |
| `log` | `{ level, message, notify? }` — `notify: true` marks a problem the player must see (see *Wire details*) |
| `plugin_status` | `{ plugin: PluginStatus }` — every plugin state change (see below) |

`AudioStatus = { type, name, sampleRate, bufferSize, inputLatencyMs, outputLatencyMs, running, hasControlPanel, panelOpen }`
(latencies are device-reported and include the buffer; `hasControlPanel`: `open_audio_panel` is available;
`panelOpen`: the driver panel is showing right now).

### Audio latency (buffer size, ASIO control panel)
- JUCE only applies a requested `buffer_size` (`set_audio_device`, `--asio-buffer`) when it is in the open device's
  `bufferSizes`; otherwise the driver's preferred size is used. Many ASIO drivers list only the size set in their own
  panel (the Yamaha Steinberg USB ASIO driver of the UR22C offers exactly one size, e.g. `[1024]`). Then the engine
  answers `devices` followed by a `log { level: "warn", notify: true }` (no `id`) naming the offered sizes.
- `open_audio_panel` shows the driver panel on the message thread (deferred: the reply goes out first). A modal
  panel runs a nested message loop, so requests keep being handled while it is open: the engine first broadcasts
  `devices` + `state` with `panelOpen: true`; until it closes `open_audio_panel` and `set_audio_device` answer
  `error` `busy` and `list_devices` does not rescan drivers (the UI disables its panel/apply buttons). After a modal
  panel the device it belonged to is reopened with the driver's preferred size (a failure is reported as
  `log { level: "error", notify: true }`). Non-modal panels (separate driver app) make the driver send a reset request when the buffer changes; the
  device restarts. Every device (re)start re-prepares the engine (full graph rebuild) and then broadcasts `devices` +
  `state` to all clients, plus a `log { level: "info" }` when buffer/rate/device changed; the new size is saved in
  `userdata/settings.json`.
- Driver-initiated restarts, stalls and errors (ARCHITECTURE §6 *Device restarts behind our back*): when the driver
  restarts the device itself (buffer/rate changed in its own settings app) the engine re-prepares and broadcasts
  `devices` + `state` plus `log { level: "info", notify: true }` when buffer/rate changed ("buffer 256 → 128"). When
  the device stops calling back for > 1.5 s (> 500 ms is only logged), reports an error, or a driver restart failed, the engine sends `log { level: "error", notify:
  true }`, reopens it with the driver's current settings (retrying with backoff) and sends `log { level: "info",
  notify: true }` once audio flows again. `restart_audio` does the same on demand (UI: "Restart audio" in the audio
  dialog). Every device event is also written to `userdata/logs/audio.log`.
- The UI warns when `outputLatencyMs` > 8 ms (top bar badge + audio dialog, "Open ASIO panel", pick 64 or 128).

`PluginStatus = { name, typeId, source: "faust"|"dll", state: "compiling"|"ok"|"error"|"faulted"|"removed",
message, kind: "instrument"|"effect"|"", version, compileMs, cached }` (docs/PLUGINS.md, ARCHITECTURE §10).
- `typeId` is `plugin:<name>` (the module type in the catalog/patch). `version` counts successful loads (0 = never
  loaded). `message` carries the Faust compiler / loader error for `error`, the fault description for `faulted`.
- Sequence on a source change (file watch) or `reload_plugin`: `compiling` → `ok` | `error`. On `error` the previous
  good version (if any) stays registered and keeps sounding. `faulted`: an instance crashed or produced NaN/Inf
  (crashed instances stay muted until the plugin is reloaded). `removed`: the plugin directory is gone.
- After `ok` and `removed` the catalog changed: clients re-request `get_catalog`. If the current patch uses the
  plugin, the engine rebuilds it (param values of surviving ids are kept) and broadcasts `state`.
- Errors and faults are also sent as `log { level: "error" }`.

### Wire details (clarifications)
- Payload fields never reuse the envelope key `type`: module typeIds travel as `module`, the audio device type as
  `device_type`.
- Replies whose type is an event (`state` for `hello`/`get_patch`/`set_patch`/structural ops, `devices`) carry the
  request `id` when the request had one; unsolicited broadcasts have no `id`.
- `ModuleInfo = { typeId, name, kind: "instrument"|"effect", category, params: ParamSpec[], uiHints? }`.
  `ParamSpec = { id, name, group, unit, min, max, def, scale: "linear"|"log"|"int"|"enum"|"bool", skewCentre,
  flags (bitmask: 1 ReadOnly, 2 Hidden, 4 NonAutomatable), choices: string[] }`.
  `uiHints = { groupOrder?: string[], front?: string[], controls?: { <param>: "knob"|"slider"|"drawbar" },
  tabs?: { name, groups: string[] }[] }`. `tabs` bundles param groups into panel tabs (fm: one per operator; groups
  not listed get their own tab); modules with > 40 visible params are tabbed per group without it; `front` becomes
  the first tab of a tabbed panel.
- `log` events with `notify: true` are problems the player should see; the UI shows only those as a toast (others,
  e.g. a transient "module 'plugin:x' unavailable" warning while a plugin compiles, or plugin errors that the
  `plugin_status` toast already shows, only go to the log). Flagged: module load failures (`Module::loadError`, e.g.
  a missing `.sfz`; reported once per module instance as `log { level: "error", notify: true, message: "<type>
  (node N): …" }`), a rejected buffer size, a failed device reopen after the driver panel.
- `state.transport` has the same keys as the `transport` request (all present) plus `pattern_edited` (bool).
  `telemetry.cpu` is 0..1.
- `telemetry.transport = { ppq, playing, step, bar }`: `step` = index of the last played step of the current pattern
  (-1 while stopped / counting in), `bar` = 0-based bar of the pattern.
- Master volume is the param `volume_db` on node `0` (`set_param { node: 0, param: "volume_db" }`).
- `devices.available[].sampleRates` / `bufferSizes`: supported values of the currently *open* device when it belongs
  to that type; empty arrays for other types (unknown until selected — probing would require loading drivers, and
  ASIO allows only one loaded driver at a time).
- `note.velocity` is 0..127 (default 100); `on:false` or velocity 0 = note off. Virtual-keyboard notes are echoed in
  `midi` events like hardware notes. `transport` changes are echoed to other clients as a `state` snapshot.
- WebSocket handshakes without an `Origin` header (non-browser clients: scripts, tests) are accepted; browser
  origins must be `http://localhost|127.0.0.1` on the WS port, the static UI port (7340) or 5173.

### Rhythm (transport, drum sequencer)
`transport` fields (all optional; omitted = unchanged):

| field | type | meaning |
|---|---|---|
| `playing` | bool | start (position → 0, optional count-in) / stop |
| `tempo` | number 20..400 | BPM (also stored as the patch `tempo`) |
| `metronome`, `metronome_volume` | bool, 0..1 | click on every beat (denominator note), accented downbeat |
| `pattern` | string | path of a pattern JSON (`presets/patterns/…`, `userdata/patterns/…`); loading applies the pattern's `tempo`, `time_sig`, `swing` and `kit` when present and stores the path in the patch `rhythm.pattern`. `""` = empty pattern |
| `drums` | bool | sequencer output on/off (position keeps running) |
| `drums_volume` | 0..1 | linear gain of the rhythm node |
| `swing` | 0..1 | delay of every 2nd step by `swing × ½ step` (0.33 ≈ triplet shuffle) |
| `time_sig` | `[num, den]` num 1..16, den 2/4/8/16 | transport meter; the current pattern is resized to it (steps kept by index) |
| `count_in` | bool | when starting: one bar of metronome before the pattern |

If the request has `pattern` or `time_sig` the new `state` goes to *all* clients (the pattern can change tempo, kit…),
otherwise to the others only. The drum kit of the sequencer is the `drums` module at node `patch.rhythm.node`
(params via `set_param`; `kit` param selects 808/909/Linn/Industrial voice models).

`Pattern` (same as the file format, see PRESETS.md *Patterns*; the engine always sends steps as number arrays):
`{ format: 1, name, description, tempo?, time_sig: [num, den], steps_per_beat, bars, swing, accent_amount,
kit?, tracks: { name, note, mute, steps: number[] /* 0 = off, 1..127 velocity */ }[], accent: number[] /* 0|1 */ }`.
`set_pattern` validates and clamps (≤ 16 tracks, steps = bars × num × steps_per_beat ≤ 256; shorter arrays are padded).

