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

export type ModuleKind = 'instrument' | 'effect';

export interface UiHints {
  /** group display order; groups not listed follow in first-appearance order */
  groupOrder?: string[];
  /** params shown on the compact "front panel" */
  front?: string[];
  /** per-param control style override */
  controls?: Record<string, 'knob' | 'slider' | 'drawbar'>;
  /**
   * Tabbed panel: groups bundled into named tabs (e.g. fm "Op 1".."Op 6"); unlisted groups get a tab each.
   * Modules with many params are tabbed per group even without this (ModulePanel AUTO_TAB_THRESHOLD).
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

export type ClientMsg =
  | HelloMsg | GetCatalogMsg | SetParamMsg | LoadPresetMsg | SavePresetMsg | ListPresetsMsg | GetPatchMsg
  | SetPatchMsg | AddLayerMsg | RemoveLayerMsg | SetZoneMsg | SetInstrumentMsg | AddFxMsg | RemoveFxMsg
  | MoveFxMsg | SetFxBypassMsg | RescanMidiMsg | NoteMsg | CcMsg | TransportMsg | ListDevicesMsg
  | SetAudioDeviceMsg | PanicMsg | ListPatternsMsg | GetPatternMsg | SetPatternMsg | SavePatternMsg
  | ListPluginsMsg | ReloadPluginMsg | OpenAudioPanelMsg;

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
export interface LogEvent { type: 'log'; level: 'debug' | 'info' | 'warn' | 'error' | string; message: string }
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

export type EngineMsg =
  | StateEvent | ParamEvent | TelemetryEvent | MidiEvent | DevicesEvent | LogEvent | ErrorReply
  | CatalogOk | LoadPresetOk | SavePresetOk | ListPresetsOk
  | PatternEvent | ListPatternsOk | GetPatternOk | SetPatternOk | SavePatternOk
  | PluginStatusEvent | ListPluginsOk | ReloadPluginOk | OpenAudioPanelOk;

export type EngineMsgType = EngineMsg['type'];
export type EngineMsgOf<T extends EngineMsgType> = Extract<EngineMsg, { type: T }>;

export const PROTOCOL_VERSION = '1';
