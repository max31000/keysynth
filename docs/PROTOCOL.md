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
| `transport` | `{ playing?, tempo?, metronome?, metronome_volume?, pattern?, drums? }` | none |
| `list_devices` | — | `devices` |
| `set_audio_device` | `{ device_type, name, sample_rate?, buffer_size? }` | `devices` |
| `panic` | — | none (all notes off, reset tails) |

## Engine → client (events)

| type | payload |
|---|---|
| `state` | `{ patch, presetPath, dirty, audio: AudioStatus, transport }` — full snapshot |
| `param` | `{ node, param, value }` — another client changed a param |
| `telemetry` | `{ cpu, xruns, voices, meters: { master:[l,r], layers:{ "<node>":[l,r] } }, readouts: { "<node>": { param: value } } }` ≤30 Hz, peaks in dBFS |
| `midi` | `{ notes: [ {note, on, velocity} ] }` batched ≤30 Hz, for keyboard highlighting |
| `devices` | `{ types: string[], current: AudioStatus, available: { type, names[], sampleRates: number[], bufferSizes: number[] }[], midiInputs: string[] }` |
| `log` | `{ level, message }` |

`AudioStatus = { type, name, sampleRate, bufferSize, inputLatencyMs, outputLatencyMs, running }`.

### Wire details (clarifications)
- Payload fields never reuse the envelope key `type`: module typeIds travel as `module`, the audio device type as
  `device_type`.
- Replies whose type is an event (`state` for `hello`/`get_patch`/`set_patch`/structural ops, `devices`) carry the
  request `id` when the request had one; unsolicited broadcasts have no `id`.
- `ModuleInfo = { typeId, name, kind: "instrument"|"effect", category, params: ParamSpec[], uiHints? }`.
  `ParamSpec = { id, name, group, unit, min, max, def, scale: "linear"|"log"|"int"|"enum"|"bool", skewCentre,
  flags (bitmask: 1 ReadOnly, 2 Hidden, 4 NonAutomatable), choices: string[] }`.
  `uiHints = { groupOrder?: string[], front?: string[], controls?: { <param>: "knob"|"slider"|"drawbar" } }`.
- `state.transport` has the same keys as the `transport` request (all present). `telemetry.cpu` is 0..1.
- Master volume is the param `volume_db` on node `0` (`set_param { node: 0, param: "volume_db" }`).
- `devices.available[].sampleRates` / `bufferSizes`: supported values of the currently *open* device when it belongs
  to that type; empty arrays for other types (unknown until selected — probing would require loading drivers, and
  ASIO allows only one loaded driver at a time).
- `note.velocity` is 0..127 (default 100); `on:false` or velocity 0 = note off. Virtual-keyboard notes are echoed in
  `midi` events like hardware notes. `transport` changes are echoed to other clients as a `state` snapshot.
- WebSocket handshakes without an `Origin` header (non-browser clients: scripts, tests) are accepted; browser
  origins must be `http://localhost|127.0.0.1` on the WS port, the static UI port (7340) or 5173.
