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

Dev builds may add one extra `web` origin with `--web-origin <origin>` / `KS_WEB_ORIGIN` (staging tests); installed
builds ignore it.

**`web` sessions** (page served by mvv42.ru) are default-deny: only the messages below are accepted, anything else
→ `error` `forbidden`.

| allowed for `web` | restriction |
|---|---|
| `hello`, `get_catalog`, `set_param`, `get_patch`, `list_presets`, `add_layer`, `remove_layer`, `set_zone`, `set_instrument`, `add_fx`, `remove_fx`, `move_fx`, `set_fx_bypass`, `note`, `cc`, `panic`, `list_patterns`, `set_pattern`, `rescan_midi`, `list_devices`, `set_audio_device`, `open_audio_panel`, `restart_audio`, `list_plugins` | none (module typeIds incl. already registered `plugin:<name>` are fine) |
| `save_preset`, `save_pattern` | none (they take a name; the engine slugifies) |
| `load_preset` | `path` is a logical path under `presets/factory/` or `userdata/presets/` |
| `get_pattern`, `transport` | `path` / `pattern` is a logical path under `presets/patterns/` or `userdata/patterns/` (or `""`) |
| `set_patch` | every path inside the patch is logical: sampler `state.sfz` under `assets/samples/`, `rhythm.pattern` as above |
| `cloud_link`, `cloud_unlink`, `sync_now`, `samples_list`, `samples_fetch`, `samples_cancel`, `samples_delete` | ids only (no paths in these messages) |
| **forbidden**: `reload_plugin` and every message not listed here (incl. future ones until added to this table) | `error` `forbidden` |

Logical path (all sessions accept them; `web` sessions accept nothing else): relative, `/`-separated, no `..` segment,
no `\`, `:`, leading `/` or control chars, starting with one of the prefixes above. `userdata/…` always means the
**active profile** (below). The engine maps logical paths to disk via AppPaths (cloud-api spec *Local storage*).

### Profiles
Unlinked: the local profile (`userdata/presets`, `userdata/patterns`). Linked to account `<accountId>`:
`userdata/accounts/<accountId>/{presets,patterns}`, addressed on the wire still as `userdata/presets/…` /
`userdata/patterns/…`. Switching profile (link, unlink, relink) broadcasts `state` (`presetPath` cleared if it pointed
into the old profile); clients re-request `list_presets` / `list_patterns`.

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
`version` = engine semver (no `v`). Right after that `state` the engine sends a `sync_status` (no `id`).

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
   `userdata/accounts/<id>/` yet) the local profile's presets and patterns are copied into it and the reply carries
   `"merged": { "presets": 12, "patterns": 3 }`. Sync starts. Re-linking the same account (re-minted token) is silent.

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
last successful full sync or `null`. `pending`: local preset changes not yet pushed. `conflicts`: conflict files
present in the profile (ordinary local presets: listed by `list_presets`, never synced; the user deletes or renames
them). Triggers: link, every 60 s, any local preset change, `sync_now`. Only presets sync in v1.

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
