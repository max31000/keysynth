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
| `add_fx` / `remove_fx` / `move_fx` | `{ layer, module, index? }` / `{ node }` / `{ node, to }` (`layer: 0` = master chain). A module of kind `midi` goes into the layer's MIDI chain (`layer.midi`, before the instrument; `error bad_request` for `layer: 0`); `move_fx` moves within the node's own chain | `state` |
| `set_fx_bypass` | `{ node, bypass }` (effects and MIDI processors) | none |
| `chord_learn` | `{ node }` (a `chord` MIDI processor) — copies the notes currently held on its input (≥ 2) into its chord memory | `chord_learn_ok { node, intervals: number[] }`; `param` events (`mem_*`, `type`) to **all** clients; `error` `no_notes` / `not_found` / `bad_request` |
| `song_list` / `song_get` / `song_save` / `song_delete` / `song_select` / `song_section` / `song_next` / `song_prev` / `song_close` / `song_control_keys` | see *Songs* | |
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
| `cloud_link` / `cloud_unlink` / `sync_now` | see *Cloud & samples* | |
| `samples_list` / `samples_fetch` / `samples_cancel` / `samples_delete` | see *Cloud & samples* | |

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
| `cloud_status`, `sync_status`, `samples_progress`, `samples_done`, `samples_error` | see *Cloud & samples* |
| `song_state`, `songs_changed` | see *Songs* |

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
- `ModuleInfo = { typeId, name, kind: "instrument"|"effect"|"midi", category, params: ParamSpec[], uiHints? }`.
  `midi` = MIDI processor (`arp`, `chord`, `hold`; ARCHITECTURE §17), placed in `layer.midi`. Its ReadOnly readouts
  (`chord`: `held_count`, `held_1`…) arrive in `telemetry.readouts` like any other.
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
  origins must be `http://localhost|127.0.0.1` on the WS port, the static UI port (7340) or 5173 (dev builds only),
  or `https://mvv42.ru` (a `web` session). Full rules: *Cloud & samples → Sessions, Origin, Host*.

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

## Songs

Setlists (ARCHITECTURE §18, file format PRESETS.md *Songs*). Song paths are logical: `presets/songs/<slug>.json`
(factory, read-only) or `userdata/songs/<slug>.json` (active profile, synced).

| type | payload | reply |
|---|---|---|
| `song_list` | — | `song_list_ok { songs: SongInfo[] }` (factory first, then user; sorted by title) |
| `song_get` | `{ path }` | `song_get_ok { path, song: Song, warnings }` |
| `song_save` | `{ song: Song, overwrite? }` → `userdata/songs/<id>.json` (`id` = `song.id` if it is a valid slug, else the slugified title) | `song_save_ok { path, warnings }`; `songs_changed` to others; `song_state` to all when it is the active song. `error` `exists` (file exists, `overwrite` not true), `bad_request` (validation, message names the field) |
| `song_delete` | `{ path }` (user songs only) | `song_delete_ok { path }`; `songs_changed` to others; deleting the active song closes it (`song_state` to all). `error` `read_only` (factory), `not_found` |
| `song_select` | `{ path, section? }` (default 0) | `song_select_ok { warnings }`, then `song_state` and `state` to all |
| `song_section` | `{ index }` | `song_section_ok { warnings }`, then `song_state` + `state` to all; `error` `no_song`, `bad_request` (index out of range) |
| `song_next` / `song_prev` | — | `song_section_ok { warnings }` + broadcasts as above; at the first/last section nothing changes (no wrap, no broadcast); `error` `no_song` |
| `song_close` | — | `song_close_ok`, `song_state` to all |
| `song_control_keys` | `{ enabled?, prev?, next? }` (global setting, `settings.json`; `prev`/`next` 0..127 or `null`) | `song_control_keys_ok`, `song_state` to all |

| event | payload |
|---|---|
| `song_state` | `{ path: string\|null, song: Song\|null, section: number /* -1 without song */, controlKeys: { enabled, prev, next, source: "song"\|"global" } /* effective */, globalControlKeys: { enabled, prev, next } }` — right after the `hello` reply (after `sync_status`), and to all on every change (select, section, close, save of the active song, control keys, profile switch) |
| `songs_changed` | `{}` — user song files changed (save, delete); clients re-request `song_list` (also after a `sync_status` that finished a sync) |

`SongInfo = { path, id, title, artist, sections: number, factory: bool }`. `Song` = the file object (PRESETS.md
*Songs*) with `id` set; the engine normalizes it (unknown keys kept).
- Selecting a section applies preset → song `time_sig` → section `pattern` → tempo → `drums` (ARCHITECTURE §18).
  A step that fails (missing preset or pattern) is reported in `warnings` and as `log { level: "error", notify: true }`;
  the other steps still apply. The preset load itself broadcasts nothing extra: the `state` after the switch covers it.
- Control keys: while a song is selected and the effective `controlKeys.enabled` is true, note-ons of `prev` /
  `next` (any channel) are not played; they switch sections (≤ 33 ms later) and produce the same broadcasts as
  `song_prev` / `song_next`. Defaults: global `{ enabled: false, prev: 21, next: 22 }` (A0, A♯0 — the two lowest
  keys of an 88-key board).
- A profile switch (link/unlink) closes a song from `userdata/songs/` (`song_state` to all).


## Cloud & samples

Design: `docs/specs/2026-10-12-cloud.md` (§11 wins). REST side, storage layout and file formats:
`docs/specs/2026-10-12-cloud-api.md`. All timestamps are ISO 8601 UTC strings (`"2026-10-12T14:03:00Z"`), sizes are
bytes, rates bytes/s. Request/reply conventions as above (`id`, `error { code, message }`).

### Sessions, Origin, Host
Checked on the WS (7341) and HTTP (7340) handshake; a failure is HTTP 403 on 7340. On 7341 the socket library has no
pre-upgrade hook, so a refused handshake is closed with code 4003 right after the upgrade and no message from it is
ever processed.
- `Host` must be exactly `127.0.0.1:<port>` or `localhost:<port>` (`<port>` = the port listened on) — DNS-rebinding
  guard. Both servers bind 127.0.0.1 only.
- `Origin` decides the session's **trust**:

| Origin | trust |
|---|---|
| absent (non-browser client: scripts, tests) | `local` |
| `http://127.0.0.1:7340`, `http://localhost:7340`, `http://127.0.0.1:7341`, `http://localhost:7341` | `local` |
| `http://127.0.0.1:5173`, `http://localhost:5173` — **dev builds only** (no `installed.flag`) | `local` |
| `https://mvv42.ru` (exact: no `www`, port or path) | `web` |
| `null`, anything else | rejected |
| — (controller relayed by the cloud hub: virtual client `r:<cid>`, no socket of its own; *Relay* below) | `remote` |

Dev builds may add one extra `web` origin with `--web-origin <origin>` / `KS_WEB_ORIGIN` (staging tests); installed
builds ignore it.

**`web` sessions** (page served by mvv42.ru) are default-deny: only the messages below are accepted, anything else
→ `error` `forbidden`.

| allowed for `web` | restriction |
|---|---|
| `hello`, `get_catalog`, `set_param`, `get_patch`, `list_presets`, `add_layer`, `remove_layer`, `set_zone`, `set_instrument`, `add_fx`, `remove_fx`, `move_fx`, `set_fx_bypass`, `note`, `cc`, `panic`, `list_patterns`, `set_pattern`, `rescan_midi`, `list_devices`, `set_audio_device`, `open_audio_panel`, `restart_audio`, `list_plugins` | none (module typeIds incl. already registered `plugin:<name>` are fine) |
| `save_preset`, `save_pattern` | none (they take a name; the engine slugifies) |
| `chord_learn`, `song_list`, `song_section`, `song_next`, `song_prev`, `song_close`, `song_control_keys` | none |
| `song_save` | none (the handler validates every section `preset` / `pattern` as a logical path or slug for all sessions) |
| `song_get`, `song_select`, `song_delete` | `path` is a logical path under `presets/songs/` or `userdata/songs/` (`song_delete`: user songs only, else `read_only`) |
| `load_preset` | `path` is a logical path under `presets/factory/` or `userdata/presets/` |
| `get_pattern`, `transport` | `path` / `pattern` is a logical path under `presets/patterns/` or `userdata/patterns/` (or `""`) |
| `set_patch` | every path inside the patch is logical: sampler `state.sfz` under `assets/samples/`, `rhythm.pattern` as above |
| `cloud_link`, `cloud_unlink`, `sync_now`, `samples_list`, `samples_fetch`, `samples_cancel`, `samples_delete` | ids only (no paths in these messages) |
| **forbidden**: `reload_plugin` and every message not listed here (incl. future ones until added to this table) | `error` `forbidden` |

Logical path (all sessions accept them; `web` sessions accept nothing else): relative, `/`-separated, no `..` segment,
no `\`, `:`, leading `/` or control chars, starting with one of the prefixes above. `userdata/…` always means the
**active profile** (below). The engine maps logical paths to disk via AppPaths (cloud-api spec *Local storage*).

**`remote` sessions** (relayed controllers) = the `web` table above, minus `cloud_link`, `cloud_unlink`,
`set_audio_device`, `open_audio_panel`, `samples_delete` (and `reload_plugin`, already forbidden) → `error`
`forbidden`; default-deny for anything new. Their `hello` reply carries `trust: "remote"`; the UI hides denied features.
Songs and `chord_learn` are allowed for `remote` exactly as for `web` (the iPad runs the setlist).

### Profiles
Unlinked: the local profile (`userdata/presets`, `userdata/patterns`, `userdata/songs`). Linked to account
`<accountId>`: `userdata/accounts/<accountId>/{presets,patterns,songs}`, addressed on the wire still as
`userdata/presets/…` / `userdata/patterns/…` / `userdata/songs/…`. Switching profile (link, unlink, relink) broadcasts
`state` (`presetPath` cleared if it pointed into the old profile) and `song_state` (a user song is closed); clients
re-request `list_presets` / `list_patterns` / `song_list`.

### Hello additions
The `state` reply to `hello` (and only that reply) carries four extra fields:
```json
{ "type": "state", "id": 1, "patch": {}, "presetPath": null, "dirty": false, "audio": {}, "transport": {},
  "version": "0.4.0",
  "engine": { "product": "keysynth", "kind": "engine", "version": "0.4.0", "pid": 15828 },
  "trust": "web",
  "cloud": { "linked": true, "accountId": "17", "accountName": "Max", "deviceId": "3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40",
             "expiresAt": "2027-04-10T12:00:00Z", "online": true, "error": null } }
```
`version` = engine semver (no `v`). Right after that `state` the engine sends a `sync_status` and a `song_state`
(no `id`).

`engine` = identity of the server: `EngineIdentity = { product: "keysynth", kind: "engine" | "mock", version: string,
pid: number }`. The real engine sends `kind: "engine"`, the dev mock (`ui/mock/mock-engine.ts`, default port 7441)
`"mock"`. The UI checks it before showing the synth (`ui/src/platform/identity.ts`):
- `product == "keysynth"`, `kind == "engine"` → ok;
- `kind == "mock"` → accepted only by dev builds (shows a `MOCK` badge); production pages show «На порту 7341
  отвечает не движок keysynth (тестовый сервер / другая программа)» with instructions;
- no `engine` (engines before 0.1.1): accepted as a legacy keysynth when `version`, `trust` and `cloud` are present,
  except the old mock's fixed `cloud.deviceId` `3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40` (= mock);
- anything else, an `error` reply or no reply to `hello` → not keysynth.

`CloudStatus = { linked: bool, accountId: string|null, accountName: string|null, deviceId: string /* always set */,
expiresAt: string|null, online: bool /* last API call succeeded */, error: string|null /* last cloud error code,
e.g. "offline", "token_expired" */ }`. Unlinked: `accountId`, `accountName`, `expiresAt` are `null`.

### Messages

| type | payload | reply |
|---|---|---|
| `cloud_link` | `{ token, replace?: bool }` | `cloud_status` (with `id`; `merged` on an account's first link on this PC), also broadcast to others |
| `cloud_unlink` | — | `cloud_status` (`linked: false`, with `id`), also broadcast |
| `sync_now` | — | `sync_now_ok`, then `sync_status` events; `error` `not_linked` |
| `samples_list` | — | `samples_list_ok` (below) |
| `samples_fetch` | `{ scope: "entry"\|"library"\|"all", target? }` (entry / library id; omitted for `all`) | `samples_fetch_ok { job, existing }` |
| `samples_cancel` | `{ job? }` (job id; omitted = all jobs) | `samples_cancel_ok { jobs: string[] }` (cancelled job ids) |
| `samples_delete` | `{ scope: "entry"\|"library"\|"all", target? }` | `samples_delete_ok { freedBytes }` |

| event (to all sessions, no `id`) | payload |
|---|---|
| `cloud_status` | `CloudStatus` fields at top level — on every change (link, unlink, online ⇄ offline, token dropped) |
| `sync_status` | `{ state, lastSyncAt, pending, conflicts, error }` — on every change, ≤ 2 Hz |
| `samples_progress` | `{ jobs: SampleJob[] }` — ≤ 5 Hz while any job is queued/running; lists all of them |
| `samples_done` | `{ job, scope, target, cancelled, entries: string[] }` — job ended (`entries`: entry ids now fully cached) |
| `samples_error` | `{ job, scope, target, code, message, path, notify: true }` — job failed and ended |

#### `cloud_link`
1. Token format must be `ksd_` + 43 base64url chars, else `error` `bad_request`.
2. `GET /api/devices/me` with the token. Network failure → `error` `offline`; 401 → `error` `token_invalid`. Nothing
   is stored on failure.
3. Already linked to a *different* account and `replace` is not `true` → `error` `account_mismatch` (message names the
   current account; nothing changes). The UI asks the user, then resends with `replace: true`.
4. Token (DPAPI) + account go to `cloud.json`, the profile switches. On the account's first link on this PC (no
   `userdata/accounts/<id>/` yet) the local profile's presets, patterns and songs are copied into it and the reply
   carries `"merged": { "presets": 12, "patterns": 3, "songs": 2 }`. Sync starts. Re-linking the same account (re-minted token) is silent.

`cloud_unlink`: `DELETE /api/devices/me` (best effort; offline ignored), token deleted, local profile active.
When any API call answers 401 (`token_expired`, `token_invalid`, `reauth_required`) the engine drops the token itself
and broadcasts `cloud_status { linked: false, error: "<code>" }`; a logged-in page re-mints and re-links (same account
→ silent). The page also re-mints when `expiresAt` is less than 30 days away.

#### `sync_status`
```json
{ "type": "sync_status", "state": "idle", "lastSyncAt": "2026-10-12T14:03:00Z", "pending": 0,
  "conflicts": [ { "slug": "warm-rhodes", "file": "userdata/presets/warm-rhodes.conflict-20261012T140300Z.json",
                   "at": "2026-10-12T14:03:00Z" } ],
  "error": null }
```
`state`: `disabled` (not linked) | `idle` | `syncing` | `offline` | `error` (`error` holds the code). `lastSyncAt`:
last successful full sync or `null`. `pending`: local preset + song changes not yet pushed. `conflicts`: conflict
files present in the profile (`file` under `userdata/presets/` or `userdata/songs/`; ordinary local presets / songs:
listed by `list_presets` / `song_list`, never synced; the user deletes or renames them). Triggers: link, every 60 s,
any local preset or song change, `sync_now`. Presets and songs sync (patterns do not).

#### `samples_list_ok`
```json
{ "type": "samples_list_ok", "id": 5,
  "manifest": { "state": "ok", "etag": "\"9c1f0a2b3c4d5e6f\"", "fetchedAt": "2026-10-12T14:00:00Z" },
  "cacheBytes": 1234567890, "freeBytes": 98765432100,
  "groups": [ { "id": "piano", "name": "Piano", "libraries": [
    { "id": "salamander", "name": "Salamander Grand Piano V3", "licence": "CC-BY-3.0", "author": "Alexander Holm",
      "attribution": "Salamander Grand Piano by Alexander Holm, CC BY 3.0. …",
      "bytes": 1960000000, "cachedBytes": 0, "status": "missing", "job": null,
      "entries": [
        { "id": "salamander/SalamanderGrandPiano-SFZ+FLAC-V3+20200602/SalamanderGrandPiano-V3+20200602.sfz",
          "name": "SalamanderGrandPiano-V3+20200602",
          "sfz": "assets/samples/salamander/SalamanderGrandPiano-SFZ+FLAC-V3+20200602/SalamanderGrandPiano-V3+20200602.sfz",
          "bytes": 1960000000, "cachedBytes": 0, "status": "missing", "job": null } ] } ] } ] }
```
- Library id = manifest library id. **Entry id = `<lib>/<sfz path relative to the library dir>`**; `sfz` = the logical
  path a patch uses = `assets/samples/<entry id>`. A sampler's `state.sfz` maps to an entry by stripping
  `assets/samples/`.
- Entry `status`: `local` (resolved from the repo `assets/` or `KS_ASSETS_DIR`; read-only: never downloaded or deleted)
  | `cached` (all files in the cache) | `partial` | `missing`. Library `status`: `local` if any entry is local, else
  `cached` if all entries are, `missing` if no file is cached, else `partial`. `job`: id of the queued/running job
  covering it, else `null`. `bytes` = sum of the unique files of the entry / library; `cachedBytes` likewise for
  cached files.
- `manifest.state`: `ok` (fresh), `stale` (cached copy; refresh failed or not linked), `none` (never fetched →
  `groups: []`, `etag`/`fetchedAt` `null`). When linked the engine revalidates the manifest (`If-None-Match`) before
  replying, 5 s timeout. Order of groups/libraries/entries = manifest order.
- `cacheBytes`: bytes of the sample cache dir; `freeBytes`: free space on its volume.

#### Jobs
- Job id = `"<scope>:<id>"` (`"entry:salamander/…sfz"`, `"library:salamander"`, `"all"`). Payload fields are `target` / `job`, never
  `id` (the envelope request id). Fetching a target whose job is already queued/running returns that job with
  `existing: true`. Overlapping jobs share files (each file downloads
  once and counts for both).
- `samples_fetch` errors: `not_linked`, `not_found` (unknown id or no manifest), `bad_request`.
- `SampleJob = { id, scope, target /* entry/library id, "" for all */, state: "queued"|"running", bytesDone,
  bytesTotal, filesDone, filesTotal, rate }`. Totals = what this job must download (already cached files excluded),
  fixed at job start; `rate` = average over the last 2 s (0 while queued).
- Queue: FIFO, auto-fetch jobs go to the front; 2 files download in parallel engine-wide; a file is retried 3× with
  backoff before its job fails. Files finished before a cancel/failure stay cached.
- After `samples_cancel` the job ends with `samples_done { cancelled: true }` (no `samples_error`).
- `samples_error.code`: `not_linked`, `offline`, `unauthorized` (token dropped, see above), `not_found` (HTTP 404),
  `http_error` (other status, in `message`), `checksum_mismatch`, `disk_full`, `bad_path` (manifest path failed
  validation), `io_error`. `path` = failing file as `<lib>/<path>`, or `null`.
- `samples_delete` removes the target's files that no other cached entry still references, then its empty dirs.
  Errors: `in_use` (a sampler in the current patch uses the entry — unload it first), `read_only` (`local` entry),
  `not_found`. Jobs covering the target are cancelled first.

#### Auto-fetch on patch load
When a patch is applied (`load_preset`, `set_patch`, `set_instrument`, any rebuild) and a sampler's `state.sfz`
(`assets/samples/<entry id>`) resolves nowhere locally:
1. Linked and the entry is in the manifest → enqueue `entry:<entry id>` at the queue front (or reuse its job). No
   missing-sfz `log`. The sampler stays silent with its ReadOnly param `loading` = 1 (in `telemetry.readouts`) while
   the job runs; normal `samples_progress` events cover it (the UI maps `state.sfz` → entry id → `job`).
2. `samples_done` → every sampler waiting for that entry loads it (`loading` stays 1 during the load, then 0).
3. `samples_error` → the sampler ends in its normal load-error state (`loading` = 0); its missing-sfz `log` is
   suppressed (the `samples_error` toast is the one notification).
4. Not linked → no error: the sampler's hidden ReadOnly `samples_missing` readout = 1 and one `log { level: "warn",
   notify: true }` per entry and run: «Сэмплы для «<preset>» не скачаны — привяжите движок к аккаунту на mvv42.ru
   (кнопка «Подключить»)». After `cloud_link` the catalog refresh wakes the sampler, which then auto-fetches (1.).
5. Linked but the server is unreachable (no catalog, or the download failed with `offline`) → `samples_missing` = 2,
   one notice «… не скачаны: нет связи с сервером, повторю позже»; the `samples_error` of such an automatic job has
   `notify: false`. The engine retries the catalog every 30 s; when it answers, waiting samplers fetch again.
6. Not in the (fresh) manifest → unchanged behaviour (missing-sfz `log { level: "error", notify: true }`).
The UI shows `samples_missing` on the sampler panel («Сэмплы не скачаны», «Скачать сэмплы (N МБ)» → `samples_fetch
{ scope: "entry" }` when linked and the entry is in the catalog). `log` with `notify` below `error` is a neutral toast.

## Relay (remote control through the cloud hub)

Design: `docs/specs/2026-10-12-remote.md` (§10 wins). Hub: `server/src/Keysynth.Api/Relay/` (`RelayHub`,
`RelayEndpoint`). Engine side: ARCHITECTURE §15 *Relay host*. The relay carries ordinary engine protocol messages
(everything above) inside an envelope; the hub never looks into `msg` beyond "is a JSON object".

### Endpoint, roles, auth
`GET /api/relay?role=host|controller[&cid=<cid>]` (public `wss://mvv42.ru/keysynth/api/relay`), WebSocket upgrade.
All checks run before the upgrade; a refusal is an HTTP status with `{"error":"<code>"}` (browsers only see a failed
socket, the engine sees the status).

| case | result |
|---|---|
| `role` missing / other | 400 `invalid_request` |
| cookie **and** `Authorization` on one request | 400 `invalid_request` |
| host: device bearer (`Authorization: Bearer ksd_…`, same checks as REST incl. `last_app_ok_at`) | ok; failures 401 `token_invalid` / `token_expired` / `reauth_required`, none 401 `unauthorized`, a cookie 403 `forbidden` |
| controller: mvv42 cookie with the keysynth grant + `Origin` exactly in `Mvv42Auth:AllowedOrigins` (`https://mvv42.ru`) | ok; `Origin` missing / foreign or a bearer → 403 `forbidden`, no/invalid cookie → 401 `unauthorized`, no grant → 403 `forbidden` |
| controller `cid` not `[A-Za-z0-9_-]{8,64}` | 400 `invalid_request` (missing `cid` → the server generates one) |
| valid, but not a WebSocket request | 400 `websocket_required` |

- **Host** = an engine; its `deviceId` is taken from the token (the `hello` value is ignored). **Controller** = a
  browser tab, identified by its client-generated `cid` (UUID). **Room** = local user id: frames never cross users.
- Controller sockets live ≤ 60 min (close 4002). At ~50 min the client refreshes its cookie (`getMe()`), opens a new
  socket with the **same `cid`** and then closes the old one: a connecting known `cid` is a **resume** — the old
  socket is closed with 4000, hosts see no `joined`/left (a paused state is reset with `resume`). The grant is
  checked at the handshake only. Host tokens are re-checked every 5 min (deleted / expired / `reauth_required` →
  close 4003); `DELETE /api/devices/{id}` and `/me` close that host at once and drop it from `hosts` (a handshake
  authenticated just before the delete is caught by the next 5-min check).
- A server-initiated close removes the socket from the room immediately (hosts list / `joined:false` / slot freed),
  without waiting for the client's close reply; the socket is aborted 5 s later if the handshake has not finished.
- A repeated `hello` only updates name/version (no second round of `controller` events).

### Envelope (JSON text frames)
```jsonc
// host -> server
{ "t": "hello", "host": { "deviceId": "…", "name": "MAX-PC", "version": "0.2.0" } }  // first; msg before it -> error hello_required
{ "t": "msg", "to": "<cid>" | "*", "except": "<cid>" /*opt*/, "lossy": true /*opt*/, "msg": { …engine message… } }
// server -> host
{ "t": "controller", "cid": "<cid>", "joined": true | false }   // after hello: one joined:true per present controller
{ "t": "msg", "from": "<cid>", "msg": { …engine request… } }
{ "t": "pause" | "resume", "cid": "<cid>" }                     // hidden tab: skip telemetry for that cid
// controller -> server
{ "t": "msg", "host": "<deviceId>", "msg": { …engine request… } }
{ "t": "pause" } { "t": "resume" }
// server -> controller
{ "t": "hosts", "hosts": [ { "deviceId": "…", "name": "MAX-PC", "version": "0.2.0", "online": true } ] }
                                                                 // on connect and on every change
{ "t": "msg", "host": "<deviceId>", "msg": { …engine reply / event… } }
// both ways
{ "t": "ping" } -> { "t": "pong" }
{ "t": "error", "code": "bad_envelope" | "hello_required" | "host_offline", "host": "…"?, "id": …? }
```
- Routing: `to:"<cid>"` → that controller (unknown cid: dropped, the host gets `joined:false` anyway); `to:"*"` →
  every controller of the room except `except`. `lossy:true` frames are also skipped for paused controllers.
- `hosts` lists every host seen in this room since the API started; `online:false` once its socket is gone (a
  replaced host goes `false` → `true` with its new `hello`; controllers re-`hello` the engine on offline → online).
- `host_offline` answers a controller `msg` to a host that is not online; `id` echoes the inner request's `id`.
- `bad_envelope`: not JSON / not an object, unknown `t`, wrong field types. The socket stays open.

### Limits and close codes
| item | limit |
|---|---|
| hosts / controllers per user | 4 / 8 (a host with the same deviceId replaces the old socket; a known cid resumes) |
| inbound message (all fragments) | host 1 MB, controller 128 KB |
| inbound rate (safety cap, burst = 1 s) | host 300 msg/s, controller 100 msg/s |
| heartbeat | server `ping` every 20 s; any inbound frame is liveness; 45 s silence → close |
| outgoing queue per socket | a lossy frame is dropped while ≥ 64 frames are queued or it would bring the queue over 1 MB; a non-lossy frame that would exceed 1024 frames / 8 MB → close 4008 |

| code | meaning | client action |
|---|---|---|
| 1000 | normal | — |
| 1001 | server shutdown | reconnect with backoff |
| 1003 | binary frame | bug |
| 1009 | inbound message over the cap | bug (engine: replies > 1 MB become `error too_large`) |
| 4000 | replaced (same host deviceId / same cid connected again) | controller: none (the new socket wins); the engine reconnects with backoff (two engines sharing one token keep replacing each other) |
| 4001 | heartbeat timeout | reconnect, re-`hello` |
| 4002 | controller socket max age (60 min) | reconnect with a refreshed cookie, same cid |
| 4003 | host token revoked / expired / reauth required | reconnect → the upgrade answers 401 → unlink |
| 4008 | non-lossy frame did not fit the outgoing queue | reconnect, re-`hello` (resync) |
| 4009 | too many hosts / controllers for this user | back off |
| 4029 | inbound rate cap | back off, fix the client |

Relayed controllers appear in the engine as virtual clients `r:<cid>` with trust `remote` (see *Sessions, Origin,
Host*). The PC browser tab with a direct engine never uses the relay.
