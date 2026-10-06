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
| `set_instrument` | `{ layer, type }` | `state` |
| `add_fx` / `remove_fx` / `move_fx` | `{ layer, type, index? }` / `{ node }` / `{ node, to }` (`layer: 0` = master chain) | `state` |
| `set_fx_bypass` | `{ node, bypass }` | none |
| `rescan_midi` | — | `devices` |
| `note` | `{ on: bool, note, velocity, channel? }` (virtual keyboard) | none |
| `cc` | `{ cc, value, channel? }` | none |
| `transport` | `{ playing?, tempo?, metronome?, metronome_volume?, pattern?, drums? }` | none |
| `list_devices` | — | `devices` |
| `set_audio_device` | `{ type, name, sample_rate?, buffer_size? }` | `devices` |
| `panic` | — | none (all notes off, reset tails) |

## Engine → client (events)

| type | payload |
|---|---|
| `state` | `{ patch, presetPath, dirty, audio: AudioStatus, transport }` — full snapshot |
| `param` | `{ node, param, value }` — another client changed a param |
| `telemetry` | `{ cpu, xruns, voices, meters: { master:[l,r], layers:{ "<node>":[l,r] } }, readouts: { "<node>": { param: value } } }` ≤30 Hz, peaks in dBFS |
| `midi` | `{ notes: [ {note, on, velocity} ] }` batched ≤30 Hz, for keyboard highlighting |
| `devices` | `{ types: string[], current: AudioStatus, available: { type, names[] }[], midiInputs: string[] }` |
| `log` | `{ level, message }` |

`AudioStatus = { type, name, sampleRate, bufferSize, inputLatencyMs, outputLatencyMs, running }`.
