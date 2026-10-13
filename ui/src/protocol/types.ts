// Typed mirror of docs/PROTOCOL.md (engine ⇄ UI WebSocket protocol) and docs/PRESETS.md (patch format).
// Keep in sync with those docs. Field names are exactly as on the wire.

// ───────────────────────────── Params / catalog (ARCHITECTURE §5.1) ─────────────────────────────

/** Wire form of ParamScale. The UI normalises case (`"Log"` and `"log"` are both accepted). */
export type ParamScale = 'linear' | 'log' | 'int' | 'enum' | 'bool';

export const ParamFlags = {
  None: 0,
  ReadOnly: 1,
  Hidden: 2,
  NonAutomatable: 4,
} as const;

export interface ParamSpec {
  /** stable snake_case, unique in module */
  id: string;
  name: string;
  group: string;
  unit: string;
  min: number;
  max: number;
  def: number;
  scale: ParamScale;
  /** Log params: value at knob centre (0 = geometric mean) */
  skewCentre: number;
  /** bitmask of ParamFlags */
  flags: number;
  /** Enum labels; value = index */
  choices: string[];
}

/** `midi` = MIDI processor (arp, chord, hold; ARCHITECTURE §17), placed in `layer.midi`. */
export type ModuleKind = 'instrument' | 'effect' | 'midi';

export interface UiHints {
  /** group display order; groups not listed follow in first-appearance order */
  groupOrder?: string[];
  /** params shown on the compact "front panel" */
  front?: string[];
  /** per-param control style override */
  controls?: Record<string, 'knob' | 'slider' | 'drawbar'>;
  /**
   * Tabbed panel: groups bundled into named tabs (e.g. fm "Op 1".."Op 6"); unlisted groups get a tab each.
   * Modules with many params are tabbed per group even without this (AUTO_TAB_THRESHOLD in lib/panelLayout.ts).
   */
  tabs?: { name: string; groups: string[] }[];
  [key: string]: unknown;
}

export interface ModuleInfo {
  typeId: string;
  name: string;
  kind: ModuleKind;
  category: string;
  params: ParamSpec[];
  uiHints?: UiHints;
}

// ───────────────────────────── Patch (PRESETS.md) ─────────────────────────────

export type NodeId = number;
/** node 0 = master chain (`layer: 0` in add_fx etc.) */
export const MASTER_NODE: NodeId = 0;

export interface Zone {
  key_lo: number;
  key_hi: number;
  vel_lo: number;
  vel_hi: number;
  transpose: number;
  /** 0 = omni, 1–16 specific */
  channel: number;
  volume_db: number;
  /** -1..1 */
  pan: number;
  mute: boolean;
  solo: boolean;
  sustain: boolean;
}

export type ParamValues = Record<string, number>;

export interface ModuleSlot {
  node?: NodeId;
  type: string;
  params: ParamValues;
  state?: Record<string, unknown>;
}

export interface FxSlot extends ModuleSlot {
  bypass: boolean;
}

export interface Layer {
  node?: NodeId;
  name: string;
  zone: Zone;
  instrument: ModuleSlot;
  fx: FxSlot[];
  /** MIDI processors in front of the instrument (PRESETS.md `layers[].midi`), ≤ 8; absent = none */
  midi?: FxSlot[];
}

export interface PatchMeta {
  name: string;
  category?: string;
  tags?: string[];
  description?: string;
  author?: string;
}

export interface Patch {
  format: number;
  meta: PatchMeta;
  tempo?: number;
  layers: Layer[];
  master: { volume_db: number; fx: FxSlot[] };
  /** Drum-sequencer kit (the `drums` module of the RhythmNode, addressed by `node`) + pattern path. */
  rhythm?: { node?: NodeId; kit?: ParamValues; pattern?: string };
}

// ───────────────────────────── Patterns (PRESETS.md "Patterns") ─────────────────────────────

export interface PatternTrack {
  name: string;
  /** MIDI note sent to the kit (GM drum map) */
  note: number;
  mute: boolean;
  /** velocity per step, 0 = off (the engine always sends arrays; files may use strings) */
  steps: number[];
}

export interface Pattern {
  format: number;
  name: string;
  description?: string;
  tempo?: number;
  /** [numerator, denominator] */
  time_sig: [number, number];
  steps_per_beat: number;
  bars: number;
  swing: number;
  accent_amount: number;
  /** drums `kit` index (0 808, 1 909, 2 Linn, 3 Industrial) */
  kit?: number;
  tracks: PatternTrack[];
  /** 0/1 per step */
  accent: number[];
}

export interface PatternEntry {
  path: string;
  name: string;
  time_sig: [number, number];
  bars: number;
  tempo: number;
  factory: boolean;
}

// ───────────────────────────── Shared status types ─────────────────────────────

export interface AudioStatus {
  type: string;
  name: string;
  sampleRate: number;
  bufferSize: number;
  inputLatencyMs: number;
  outputLatencyMs: number;
  running: boolean;
  /** the driver has its own settings panel (ASIO): `open_audio_panel` */
  hasControlPanel?: boolean;
  /** the driver's control panel is open right now (open_audio_panel / set_audio_device answer `busy`) */
  panelOpen?: boolean;
}

/** Transport state, same keys as the `transport` request (+ `pattern_edited`). */
export interface TransportState {
  playing: boolean;
  tempo: number;
  metronome: boolean;
  metronome_volume: number;
  /** current pattern path ("" = none / unsaved empty) */
  pattern?: string;
  pattern_edited?: boolean;
  /** drum sequencer output on/off */
  drums?: boolean;
  /** 0..1 linear */
  drums_volume?: number;
  /** 0..1 */
  swing?: number;
  time_sig?: [number, number];
  count_in?: boolean;
}

/** Transport fields a client may send (`pattern_edited` is engine-owned). */
export type TransportRequest = Partial<Omit<TransportState, 'pattern_edited'>>;

// ───────────────────────────── Songs (PRESETS.md "Songs", PROTOCOL.md "Songs") ─────────────────────────────

/** `prev` / `next`: MIDI note 0..127 or null (none). */
export interface ControlKeys {
  enabled: boolean;
  prev: number | null;
  next: number | null;
}
/** Effective control keys in `song_state` (the song's override or the global setting). */
export interface EffectiveControlKeys extends ControlKeys {
  source: 'song' | 'global';
}

export interface SongSection {
  /** 1..60 chars */
  name: string;
  /** logical preset path or bare user preset slug; absent = keep the current sound */
  preset?: string;
  /** logical pattern path; `""` = empty pattern; absent = keep */
  pattern?: string;
  /** true = drums on + transport playing; false = transport stopped; absent = unchanged */
  drums?: boolean;
  /** BPM override (else the song tempo) */
  tempo?: number;
  /** ≤ 2000 chars */
  notes?: string;
  /** reserved (not implemented; kept verbatim) */
  bars?: unknown;
  start_bar?: unknown;
  notation_part?: unknown;
  /** unknown keys are kept verbatim by the engine */
  [key: string]: unknown;
}

export interface Song {
  format: number;
  /** slug of the file name */
  id: string;
  /** 1..200 chars */
  title: string;
  artist?: string;
  /** BPM 20..400; absent = keep the current tempo */
  tempo?: number;
  time_sig?: [number, number];
  /** free text ("E minor") */
  key?: string;
  /** ≤ 2000 chars */
  notes?: string;
  /** per-song override of the global control keys; absent = global */
  control_keys?: ControlKeys;
  /** 1..64, played in order */
  sections: SongSection[];
  /** reserved (not implemented; kept verbatim) */
  notation?: unknown;
  backing?: unknown;
  [key: string]: unknown;
}

export interface SongInfo {
  /** `presets/songs/<slug>.json` (factory) or `userdata/songs/<slug>.json` */
  path: string;
  id: string;
  title: string;
  artist: string;
  /** number of sections */
  sections: number;
  factory: boolean;
}

/** `song_state` payload (without `type`). */
export interface SongState {
  path: string | null;
  song: Song | null;
  /** -1 without song */
  section: number;
  controlKeys: EffectiveControlKeys;
  globalControlKeys: ControlKeys;
}

export interface PresetEntry {
  path: string;
  name: string;
  category: string;
  tags: string[];
  factory: boolean;
}

export type StereoPeak = [number, number];

// ───────────────────────────── Client → engine ─────────────────────────────

export interface HelloMsg { type: 'hello'; id?: number; client: 'ui'; version: string }
export interface GetCatalogMsg { type: 'get_catalog'; id?: number }
export interface SetParamMsg { type: 'set_param'; id?: number; node: NodeId; param: string; value: number }
export interface LoadPresetMsg { type: 'load_preset'; id?: number; path: string }
export interface SavePresetMsg { type: 'save_preset'; id?: number; name: string; category?: string; overwrite?: boolean }
export interface ListPresetsMsg { type: 'list_presets'; id?: number }
export interface GetPatchMsg { type: 'get_patch'; id?: number }
export interface SetPatchMsg { type: 'set_patch'; id?: number; patch: Patch }
export interface AddLayerMsg { type: 'add_layer'; id?: number; index?: number }
export interface RemoveLayerMsg { type: 'remove_layer'; id?: number; layer: NodeId }
export interface SetZoneMsg { type: 'set_zone'; id?: number; layer: NodeId; zone: Partial<Zone> }
export interface RemoveFxMsg { type: 'remove_fx'; id?: number; node: NodeId }
export interface MoveFxMsg { type: 'move_fx'; id?: number; node: NodeId; to: number }
export interface SetFxBypassMsg { type: 'set_fx_bypass'; id?: number; node: NodeId; bypass: boolean }
export interface RescanMidiMsg { type: 'rescan_midi'; id?: number }
export interface NoteMsg { type: 'note'; id?: number; on: boolean; note: number; velocity: number; channel?: number }
export interface CcMsg { type: 'cc'; id?: number; cc: number; value: number; channel?: number }
export interface TransportMsg extends TransportRequest { type: 'transport'; id?: number }
export interface ListPatternsMsg { type: 'list_patterns'; id?: number }
export interface GetPatternMsg { type: 'get_pattern'; id?: number; path?: string }
export interface SetPatternMsg { type: 'set_pattern'; id?: number; pattern: Pattern }
export interface SavePatternMsg { type: 'save_pattern'; id?: number; name: string; overwrite?: boolean }
export interface ListDevicesMsg { type: 'list_devices'; id?: number }
export interface PanicMsg { type: 'panic'; id?: number }
export interface ListPluginsMsg { type: 'list_plugins'; id?: number }
/** `name` is the plugin directory name (`plugins/<name>/`), not a path. */
export interface ReloadPluginMsg { type: 'reload_plugin'; id?: number; name: string }
/** Copies the notes held on a `chord` MIDI processor's input (≥ 2) into its chord memory. */
export interface ChordLearnMsg { type: 'chord_learn'; id?: number; node: NodeId }

// ── Songs ──
export interface SongListMsg { type: 'song_list'; id?: number }
export interface SongGetMsg { type: 'song_get'; id?: number; path: string }
export interface SongSaveMsg { type: 'song_save'; id?: number; song: Song; overwrite?: boolean }
export interface SongDeleteMsg { type: 'song_delete'; id?: number; path: string }
export interface SongSelectMsg { type: 'song_select'; id?: number; path: string; section?: number }
export interface SongSectionMsg { type: 'song_section'; id?: number; index: number }
export interface SongNextMsg { type: 'song_next'; id?: number }
export interface SongPrevMsg { type: 'song_prev'; id?: number }
export interface SongCloseMsg { type: 'song_close'; id?: number }
export interface SongControlKeysMsg { type: 'song_control_keys'; id?: number; enabled?: boolean; prev?: number | null; next?: number | null }

/**
 * `set_instrument`, `add_fx` and `set_audio_device` would carry a payload field named `type`, which collides with
 * the envelope `type`. Per the PROTOCOL.md clarification the module typeId travels as `module` and the audio device
 * type as `device_type`.
 */
export interface SetInstrumentMsg { type: 'set_instrument'; id?: number; layer: NodeId; module: string }
export interface AddFxMsg { type: 'add_fx'; id?: number; layer: NodeId; module: string; index?: number }
/** `set_audio_device` has the same collision: device type travels as `device_type`. */
export interface SetAudioDeviceMsg { type: 'set_audio_device'; id?: number; device_type: string; name: string; sample_rate?: number; buffer_size?: number }
/** Opens the driver's own control panel (ASIO buffer size); the device restarts → `devices` + `state` broadcast. */
export interface OpenAudioPanelMsg { type: 'open_audio_panel'; id?: number }
/** Closes + reopens the audio device with the driver's current buffer/rate → `devices`, then `state` to all. */
export interface RestartAudioMsg { type: 'restart_audio'; id?: number }

// ── Cloud & samples (PROTOCOL.md "Cloud & samples") ──

/** Device token from `POST /api/devices` (`ksd_` + 43 base64url chars). `replace` confirms switching accounts. */
export interface CloudLinkMsg { type: 'cloud_link'; id?: number; token: string; replace?: boolean }
export interface CloudUnlinkMsg { type: 'cloud_unlink'; id?: number }
export interface SyncNowMsg { type: 'sync_now'; id?: number }
export interface SamplesListMsg { type: 'samples_list'; id?: number }
export type SampleScope = 'entry' | 'library' | 'all';
/** `target` = entry id (`<lib>/<sfz path>`) or library id; omitted for `all`. */
export interface SamplesFetchMsg { type: 'samples_fetch'; id?: number; scope: SampleScope; target?: string }
/** `job` = job id (`"<scope>:<target>"` / `"all"`); omitted = cancel all jobs. */
export interface SamplesCancelMsg { type: 'samples_cancel'; id?: number; job?: string }
export interface SamplesDeleteMsg { type: 'samples_delete'; id?: number; scope: SampleScope; target?: string }

export type ClientMsg =
  | HelloMsg | GetCatalogMsg | SetParamMsg | LoadPresetMsg | SavePresetMsg | ListPresetsMsg | GetPatchMsg
  | SetPatchMsg | AddLayerMsg | RemoveLayerMsg | SetZoneMsg | SetInstrumentMsg | AddFxMsg | RemoveFxMsg
  | MoveFxMsg | SetFxBypassMsg | RescanMidiMsg | NoteMsg | CcMsg | TransportMsg | ListDevicesMsg
  | SetAudioDeviceMsg | PanicMsg | ListPatternsMsg | GetPatternMsg | SetPatternMsg | SavePatternMsg
  | ListPluginsMsg | ReloadPluginMsg | OpenAudioPanelMsg | RestartAudioMsg
  | CloudLinkMsg | CloudUnlinkMsg | SyncNowMsg | SamplesListMsg | SamplesFetchMsg | SamplesCancelMsg
  | SamplesDeleteMsg | ChordLearnMsg
  | SongListMsg | SongGetMsg | SongSaveMsg | SongDeleteMsg | SongSelectMsg | SongSectionMsg | SongNextMsg
  | SongPrevMsg | SongCloseMsg | SongControlKeysMsg;

export type ClientMsgType = ClientMsg['type'];

// ───────────────────────────── Engine → client ─────────────────────────────

export interface StateEvent {
  type: 'state';
  id?: number;
  patch: Patch;
  presetPath: string | null;
  dirty: boolean;
  audio: AudioStatus;
  transport: TransportState;
  /** only in the reply to `hello` (see HelloStateFields) */
  version?: string;
  engine?: EngineIdentity;
  trust?: SessionTrust;
  cloud?: CloudStatus;
}
export interface ParamEvent { type: 'param'; node: NodeId; param: string; value: number }
export interface TelemetryEvent {
  type: 'telemetry';
  /** 0..1 (callback time / buffer duration) */
  cpu: number;
  xruns: number;
  voices: number;
  meters: { master: StereoPeak; layers: Record<string, StereoPeak> };
  readouts: Record<string, Record<string, number>>;
  /** step = last played pattern step (-1 stopped / count-in), bar = its bar */
  transport?: { ppq: number; playing: boolean; step: number; bar: number };
}
export interface MidiNote { note: number; on: boolean; velocity: number }
export interface MidiEvent { type: 'midi'; notes: MidiNote[] }
export interface DevicesEvent {
  type: 'devices';
  id?: number;
  types: string[];
  current: AudioStatus;
  /** sampleRates / bufferSizes: supported values of the open device when of this type, else empty */
  available: { type: string; names: string[]; sampleRates?: number[]; bufferSizes?: number[] }[];
  midiInputs: string[];
}
export interface LogEvent {
  type: 'log';
  level: 'debug' | 'info' | 'warn' | 'error' | string;
  message: string;
  /** a problem the player must see (failed module load, rejected buffer size, device reopen failure): toast it */
  notify?: boolean;
}
export interface ErrorReply { type: 'error'; id?: number; code: string; message: string }

/** Hot-reloaded DSP plugin (docs/PROTOCOL.md `PluginStatus`, docs/PLUGINS.md). */
export type PluginState = 'compiling' | 'ok' | 'error' | 'faulted' | 'removed';
export interface PluginStatus {
  name: string;
  /** `plugin:<name>` — module type in the catalog / patch */
  typeId: string;
  source: 'faust' | 'dll';
  state: PluginState;
  /** compiler / loader error (`error`) or fault description (`faulted`) */
  message: string;
  kind: 'instrument' | 'effect' | '';
  /** successful loads so far (0 = never loaded) */
  version: number;
  compileMs: number;
  cached: boolean;
}
export interface PluginStatusEvent { type: 'plugin_status'; plugin: PluginStatus }
export interface ListPluginsOk {
  type: 'list_plugins_ok';
  id?: number;
  plugins: PluginStatus[];
  faust: { available: boolean; version: string; reason: string };
}
export interface ReloadPluginOk { type: 'reload_plugin_ok'; id?: number; name: string }
export interface OpenAudioPanelOk { type: 'open_audio_panel_ok'; id?: number }
/** `intervals`: semitones above the lowest learned note. */
export interface ChordLearnOk { type: 'chord_learn_ok'; id?: number; node: NodeId; intervals: number[] }

// ── Songs ──
export interface SongListOk { type: 'song_list_ok'; id?: number; songs: SongInfo[] }
export interface SongGetOk { type: 'song_get_ok'; id?: number; path: string; song: Song; warnings?: string[] }
export interface SongSaveOk { type: 'song_save_ok'; id?: number; path: string; warnings?: string[] }
export interface SongDeleteOk { type: 'song_delete_ok'; id?: number; path: string }
export interface SongSelectOk { type: 'song_select_ok'; id?: number; warnings?: string[] }
/** Reply to `song_section`, `song_next`, `song_prev`. */
export interface SongSectionOk { type: 'song_section_ok'; id?: number; warnings?: string[] }
export interface SongCloseOk { type: 'song_close_ok'; id?: number }
export interface SongControlKeysOk { type: 'song_control_keys_ok'; id?: number }
/** After the hello reply (after `sync_status`) and to all on every change. */
export interface SongStateEvent extends SongState { type: 'song_state'; id?: number }
/** User song files changed: re-request `song_list`. */
export interface SongsChangedEvent { type: 'songs_changed' }

export interface CatalogOk { type: 'catalog_ok'; id?: number; modules: ModuleInfo[] }
export interface LoadPresetOk { type: 'load_preset_ok'; id?: number }
export interface SavePresetOk { type: 'save_preset_ok'; id?: number; path: string }
export interface ListPresetsOk { type: 'list_presets_ok'; id?: number; presets: PresetEntry[] }
/** Current pattern snapshot (also the `get_pattern_ok` payload). */
export interface PatternSnapshot { path: string; edited: boolean; pattern: Pattern }
/** Another client changed the current pattern. */
export interface PatternEvent extends PatternSnapshot { type: 'pattern' }
export interface ListPatternsOk { type: 'list_patterns_ok'; id?: number; patterns: PatternEntry[] }
export interface GetPatternOk extends PatternSnapshot { type: 'get_pattern_ok'; id?: number }
export interface SetPatternOk { type: 'set_pattern_ok'; id?: number; warnings?: string[] }
export interface SavePatternOk { type: 'save_pattern_ok'; id?: number; path: string }

// ── Cloud & samples (PROTOCOL.md "Cloud & samples") ──

/**
 * `web` = page from https://mvv42.ru (restricted message set); `local` = local UI / no Origin; `remote` = a
 * controller reaching the engine through the relay (web set minus the remote deny list, platform/trust.ts).
 */
export type SessionTrust = 'local' | 'web' | 'remote';

export interface CloudStatus {
  linked: boolean;
  /** keysynth account id (string); null when unlinked */
  accountId: string | null;
  accountName: string | null;
  /** UUID v4, always set */
  deviceId: string;
  /** token expiry (ISO 8601 UTC); null when unlinked */
  expiresAt: string | null;
  /** last API call succeeded */
  online: boolean;
  /** last cloud error code (`offline`, `token_expired`, …) */
  error: string | null;
}
/** Reply to `cloud_link` / `cloud_unlink` (with `id`) and broadcast on every change (no `id`). */
export interface CloudStatusEvent extends CloudStatus {
  type: 'cloud_status';
  id?: number;
  /** only in the reply to an account's first link on this PC */
  merged?: { presets: number; patterns: number; songs?: number };
}

/** `state` reply to `hello` carries these extra fields (only that reply). */
/** Who answers on the engine port (PROTOCOL.md "Hello additions"); checked by `platform/identity.ts`. */
export interface EngineIdentity {
  product: 'keysynth';
  kind: 'engine' | 'mock';
  version: string;
  pid: number;
}

export interface HelloStateFields {
  /** engine semver, no `v` */
  version: string;
  /** absent in engines before 0.1.1 */
  engine?: EngineIdentity;
  trust: SessionTrust;
  cloud: CloudStatus;
}
export type HelloStateEvent = StateEvent & HelloStateFields;

export type SyncState = 'disabled' | 'idle' | 'syncing' | 'offline' | 'error';
export interface SyncConflict {
  slug: string;
  /** logical path of the kept local copy, `userdata/presets/<slug>.conflict-<ts>.json` */
  file: string;
  at: string;
}
export interface SyncStatusEvent {
  type: 'sync_status';
  state: SyncState;
  lastSyncAt: string | null;
  /** local preset changes not yet pushed */
  pending: number;
  conflicts: SyncConflict[];
  error: string | null;
}
export interface SyncNowOk { type: 'sync_now_ok'; id?: number }

export type SampleStatus = 'local' | 'cached' | 'partial' | 'missing';
export interface SampleEntry {
  /** `<lib>/<sfz path relative to the library dir>` */
  id: string;
  name: string;
  /** logical path used in patches: `assets/samples/<id>` */
  sfz: string;
  bytes: number;
  cachedBytes: number;
  status: SampleStatus;
  /** queued/running job covering this entry */
  job: string | null;
}
export interface SampleLibrary {
  id: string;
  name: string;
  licence: string;
  author: string;
  attribution: string;
  bytes: number;
  cachedBytes: number;
  status: SampleStatus;
  /** job of this library (or covering it) */
  job: string | null;
  entries: SampleEntry[];
}
export interface SampleGroup { id: string; name: string; libraries: SampleLibrary[] }
export interface SamplesManifestState {
  state: 'ok' | 'stale' | 'none';
  etag: string | null;
  fetchedAt: string | null;
}
export interface SamplesListOk {
  type: 'samples_list_ok';
  id?: number;
  manifest: SamplesManifestState;
  cacheBytes: number;
  freeBytes: number;
  groups: SampleGroup[];
}
export interface SamplesFetchOk { type: 'samples_fetch_ok'; id?: number; job: string; existing: boolean }
export interface SamplesCancelOk { type: 'samples_cancel_ok'; id?: number; jobs: string[] }
export interface SamplesDeleteOk { type: 'samples_delete_ok'; id?: number; freedBytes: number }

export interface SampleJob {
  /** `"<scope>:<target>"` or `"all"` */
  id: string;
  scope: SampleScope;
  /** entry / library id, `""` for `all` */
  target: string;
  state: 'queued' | 'running';
  bytesDone: number;
  bytesTotal: number;
  filesDone: number;
  filesTotal: number;
  /** bytes/s, average over the last 2 s */
  rate: number;
}
/** ≤ 5 Hz while any job is queued/running. */
export interface SamplesProgressEvent { type: 'samples_progress'; jobs: SampleJob[] }
export interface SamplesDoneEvent {
  type: 'samples_done';
  job: string;
  scope: SampleScope;
  target: string;
  cancelled: boolean;
  /** entry ids now fully cached */
  entries: string[];
}
export type SamplesErrorCode =
  | 'not_linked' | 'offline' | 'unauthorized' | 'not_found' | 'http_error' | 'checksum_mismatch'
  | 'disk_full' | 'bad_path' | 'io_error';
export interface SamplesErrorEvent {
  type: 'samples_error';
  job: string;
  scope: SampleScope;
  target: string;
  code: SamplesErrorCode | string;
  message: string;
  /** failing file `<lib>/<path>` */
  path: string | null;
  notify: true;
}

export type EngineMsg =
  | StateEvent | ParamEvent | TelemetryEvent | MidiEvent | DevicesEvent | LogEvent | ErrorReply
  | CatalogOk | LoadPresetOk | SavePresetOk | ListPresetsOk
  | PatternEvent | ListPatternsOk | GetPatternOk | SetPatternOk | SavePatternOk
  | PluginStatusEvent | ListPluginsOk | ReloadPluginOk | OpenAudioPanelOk
  | CloudStatusEvent | SyncStatusEvent | SyncNowOk | SamplesListOk | SamplesFetchOk | SamplesCancelOk
  | SamplesDeleteOk | SamplesProgressEvent | SamplesDoneEvent | SamplesErrorEvent | ChordLearnOk
  | SongListOk | SongGetOk | SongSaveOk | SongDeleteOk | SongSelectOk | SongSectionOk | SongCloseOk
  | SongControlKeysOk | SongStateEvent | SongsChangedEvent;

export type EngineMsgType = EngineMsg['type'];
export type EngineMsgOf<T extends EngineMsgType> = Extract<EngineMsg, { type: T }>;

export const PROTOCOL_VERSION = '1';
