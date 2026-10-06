// Zustand store: patch, catalog, presets, devices/audio, transport, connection, selection.
// Param/zone changes are optimistic and coalesced per animation frame before being sent.

import { createStore, type StoreApi } from 'zustand/vanilla';
import { EngineClient, type ConnectionStatus } from '../protocol/client';
import {
  MASTER_NODE,
  PROTOCOL_VERSION,
  type AudioStatus,
  type DevicesEvent,
  type EngineMsg,
  type LogEvent,
  type ModuleInfo,
  type NodeId,
  type Patch,
  type Pattern,
  type PatternEntry,
  type PatternSnapshot,
  type PluginStatus,
  type PresetEntry,
  type TransportRequest,
  type TransportState,
  type Zone,
} from '../protocol/types';
import { normalizePattern, setTimeSig } from '../lib/stepGrid';
import { applyBypass, applyMoveFx, applyParam, applyZone, findLayer, findModule } from './patchOps';
import { clearNotes, pushMidi, pushTelemetry } from './liveBus';

export interface Stats {
  cpu: number;
  xruns: number;
  voices: number;
}

export interface EngineState {
  connection: { status: ConnectionStatus; attempt: number; url: string };
  catalog: ModuleInfo[];
  catalogMap: Record<string, ModuleInfo>;
  patch: Patch | null;
  presetPath: string | null;
  dirty: boolean;
  presets: PresetEntry[];
  audio: AudioStatus | null;
  devices: DevicesEvent | null;
  transport: TransportState;
  stats: Stats;
  selectedLayer: NodeId | null;
  selectedModule: NodeId | null;
  logs: LogEvent[];
  lastError: string | null;
  /** current drum pattern (engine snapshot + optimistic step edits) */
  pattern: PatternSnapshot | null;
  patterns: PatternEntry[];
  /** Hot-reloaded plugins by name (docs/PLUGINS.md). */
  plugins: Record<string, PluginStatus>;
  /** Latest plugin status change, shown as a toast until dismissed (ok/removed auto-hide). */
  pluginNotice: PluginStatus | null;

  /** Reducer for one engine message (exposed for tests). */
  handle(msg: EngineMsg): void;
  setParam(node: NodeId, param: string, value: number): void;
  setZone(layer: NodeId, zone: Partial<Zone>): void;
  flush(): void;
  setBypass(node: NodeId, bypass: boolean): void;
  /** Called by bindClient on connection status changes. */
  connectionChanged(status: ConnectionStatus): void;
  selectLayer(layer: NodeId): void;
  selectModule(node: NodeId, layer?: NodeId): void;
  setTransport(t: TransportRequest): void;
  /** Load a pattern file as the current pattern (transport `pattern`). */
  loadPattern(path: string): void;
  refreshPatterns(): Promise<void>;
  fetchPattern(): Promise<void>;
  /** Optimistic pattern edit; coalesced into one `set_pattern` per flush. */
  editPattern(fn: (p: Pattern) => Pattern): void;
  savePattern(name: string, overwrite?: boolean): Promise<string | null>;
  loadPreset(path: string): Promise<void>;
  savePreset(name: string, category?: string, overwrite?: boolean): Promise<string | null>;
  refreshPresets(): Promise<void>;
  addLayer(): Promise<void>;
  removeLayer(layer: NodeId): Promise<void>;
  setInstrument(layer: NodeId, module: string): Promise<void>;
  addFx(layer: NodeId, module: string, index?: number): Promise<void>;
  removeFx(node: NodeId): Promise<void>;
  moveFx(node: NodeId, to: number): Promise<void>;
  note(on: boolean, note: number, velocity: number): void;
  panic(): void;
  listDevices(): Promise<void>;
  setAudioDevice(deviceType: string, name: string, sampleRate?: number, bufferSize?: number): Promise<void>;
  rescanMidi(): Promise<void>;
  /** ASIO driver panel (the only way to change the buffer of drivers like the Yamaha Steinberg USB ASIO). */
  openAudioPanel(): Promise<void>;
  /** `restart_audio`: reopen the device with the driver's current settings (sound broke after a driver change). */
  restartAudio(): Promise<void>;
  clearError(): void;
  refreshPlugins(): Promise<void>;
  reloadPlugin(name: string): Promise<void>;
  dismissPluginNotice(): void;
}

export interface StoreDeps {
  client: EngineClient;
  /** schedule a flush (default: requestAnimationFrame, fallback setTimeout 16 ms) */
  schedule?: (fn: () => void) => void;
  /** ms between stats (cpu/xruns) React updates */
  statsIntervalMs?: number;
  now?: () => number;
}

const DEFAULT_TRANSPORT: TransportState = { playing: false, tempo: 120, metronome: false, metronome_volume: 0.5 };

/** Next animation frame; falls back to a 16 ms timer when rAF is unavailable or the page is hidden (rAF paused). */
const defaultSchedule = (fn: () => void) => {
  const hidden = typeof document !== 'undefined' && document.visibilityState === 'hidden';
  if (typeof requestAnimationFrame !== 'undefined' && !hidden) requestAnimationFrame(() => fn());
  else setTimeout(fn, 16);
};

export type EngineStore = StoreApi<EngineState>;

export function createEngineStore(deps: StoreDeps): EngineStore {
  const { client } = deps;
  const schedule = deps.schedule ?? defaultSchedule;
  const statsInterval = deps.statsIntervalMs ?? 250;
  const now = deps.now ?? (() => Date.now());

  // Pending outgoing changes, coalesced until the next flush. One policy for all fire-and-forget edits
  // (params, zones, bypass, transport): an edit stays pending until it was actually written to an open, synced
  // socket. While offline it is kept and shown optimistically; after (re)connect the first `state` prunes edits
  // whose target no longer exists (or now holds a different module type) and re-sends the rest.
  // Loading a preset drops pending patch edits (they belonged to the old patch).
  const pendingParams = new Map<string, { node: NodeId; param: string; value: number; type?: string }>();
  const pendingZones = new Map<NodeId, Partial<Zone>>();
  const pendingBypass = new Map<NodeId, { bypass: boolean; type?: string }>();
  let pendingTransport: TransportRequest | null = null;
  let pendingPattern: Pattern | null = null;
  /** set_pattern requests not yet acknowledged (snapshots older than them are ignored) */
  let patternInFlight = 0;
  let patternFetch: Promise<void> | null = null;
  let flushScheduled = false;
  /** true once a `state` arrived on the current connection */
  let synced = false;
  let lastStatsAt = -Infinity;

  const hasPending = () =>
    pendingParams.size > 0 || pendingZones.size > 0 || pendingBypass.size > 0 || pendingTransport !== null || pendingPattern !== null;

  /** Drop pending edits whose target vanished or changed module type in this snapshot. */
  const prunePending = (patch: Patch) => {
    const alive = (node: NodeId, type?: string) => {
      if (node === MASTER_NODE) return true;
      const m = findModule(patch, node);
      return !!m && (type === undefined || m.slot.type === type);
    };
    for (const [k, e] of pendingParams) if (!alive(e.node, e.type)) pendingParams.delete(k);
    for (const [n, e] of pendingBypass) if (!alive(n, e.type)) pendingBypass.delete(n);
    for (const l of [...pendingZones.keys()]) if (!findLayer(patch, l)) pendingZones.delete(l);
  };

  const dropPatchEdits = () => {
    pendingParams.clear();
    pendingZones.clear();
    pendingBypass.clear();
  };

  /** Re-apply not-yet-sent optimistic edits on top of an incoming snapshot. */
  const withPending = (patch: Patch): Patch => {
    let p = patch;
    for (const { node, param, value } of pendingParams.values()) p = applyParam(p, node, param, value);
    for (const [layer, zone] of pendingZones) p = applyZone(p, layer, zone);
    for (const [node, { bypass }] of pendingBypass) p = applyBypass(p, node, bypass);
    return p;
  };

  const reportError = (e: unknown) => {
    store.setState({ lastError: e instanceof Error ? e.message : String(e) });
  };

  const store = createStore<EngineState>()((set, get) => {
    const scheduleFlush = () => {
      if (flushScheduled) return;
      flushScheduled = true;
      schedule(() => get().flush());
    };

    const fixSelection = (patch: Patch) => {
      let { selectedLayer, selectedModule } = get();
      if (selectedLayer === null || !findLayer(patch, selectedLayer)) selectedLayer = patch.layers[0]?.node ?? null;
      if (selectedModule === null || !findModule(patch, selectedModule)) {
        selectedModule = (selectedLayer !== null ? findLayer(patch, selectedLayer)?.instrument.node : undefined) ?? null;
      }
      return { selectedLayer, selectedModule };
    };

    const structural = async (fn: () => Promise<unknown>) => {
      try {
        await fn();
      } catch (e) {
        reportError(e);
      }
    };

    return {
      connection: { status: client.status, attempt: 0, url: client.url },
      catalog: [],
      catalogMap: {},
      patch: null,
      presetPath: null,
      dirty: false,
      presets: [],
      audio: null,
      devices: null,
      transport: DEFAULT_TRANSPORT,
      stats: { cpu: 0, xruns: 0, voices: 0 },
      selectedLayer: null,
      selectedModule: null,
      logs: [],
      lastError: null,
      pattern: null,
      patterns: [],
      plugins: {},
      pluginNotice: null,

      handle(msg) {
        switch (msg.type) {
          case 'state': {
            prunePending(msg.patch);
            const patch = withPending(msg.patch);
            set({
              patch,
              presetPath: msg.presetPath,
              dirty: msg.dirty || hasPending(),
              audio: msg.audio,
              transport: { ...DEFAULT_TRANSPORT, ...msg.transport, ...pendingTransport },
              ...fixSelection(patch),
            });
            synced = true;
            if (hasPending()) scheduleFlush();
            // The engine changed the current pattern (load, meter, preset) → fetch it.
            const tp = msg.transport.pattern;
            const cur = get().pattern;
            const ts = msg.transport.time_sig;
            if (
              tp !== undefined &&
              !pendingPattern &&
              (!cur || cur.path !== tp || cur.edited !== !!msg.transport.pattern_edited ||
                (ts && (cur.pattern.time_sig[0] !== ts[0] || cur.pattern.time_sig[1] !== ts[1])))
            ) {
              void get().fetchPattern();
            }
            break;
          }
          case 'pattern':
          case 'get_pattern_ok':
            if (!pendingPattern && patternInFlight === 0)
              set({ pattern: { path: msg.path, edited: msg.edited, pattern: normalizePattern(msg.pattern) } });
            break;
          case 'list_patterns_ok':
            set({ patterns: msg.patterns });
            break;
          case 'param': {
            const { patch } = get();
            if (patch) set({ patch: applyParam(patch, msg.node, msg.param, msg.value), dirty: true });
            break;
          }
          case 'catalog_ok': {
            const catalogMap: Record<string, ModuleInfo> = {};
            for (const m of msg.modules) catalogMap[m.typeId] = m;
            set({ catalog: msg.modules, catalogMap });
            break;
          }
          case 'list_presets_ok':
            set({ presets: msg.presets });
            break;
          case 'devices':
            set({ devices: msg, audio: msg.current });
            break;
          case 'telemetry': {
            pushTelemetry(msg);
            const t = now();
            if (t - lastStatsAt >= statsInterval) {
              lastStatsAt = t;
              const s = get().stats;
              if (s.cpu !== msg.cpu || s.xruns !== msg.xruns || s.voices !== msg.voices) {
                set({ stats: { cpu: msg.cpu, xruns: msg.xruns, voices: msg.voices } });
              }
            }
            break;
          }
          case 'midi':
            pushMidi(msg.notes);
            break;
          case 'log':
            set({ logs: [...get().logs.slice(-49), msg] });
            // Only problems the engine flags for the player (`notify`) become a toast; transient warnings (a
            // plugin module unavailable while it compiles) and plugin errors (plugin toast) stay in the log.
            if (msg.notify) set({ lastError: msg.message });
            break;
          case 'error':
            if (msg.id === undefined) set({ lastError: `${msg.code}: ${msg.message}` });
            break;
          case 'plugin_status': {
            const p = msg.plugin;
            const plugins = { ...get().plugins };
            if (p.state === 'removed') delete plugins[p.name];
            else plugins[p.name] = p;
            set({ plugins, pluginNotice: p });
            // A (re)loaded or removed plugin changes the catalog (new params, new/gone module type).
            if (p.state === 'ok' || p.state === 'removed') {
              void client.request({ type: 'get_catalog' }, { expect: 'catalog_ok' }).catch(() => undefined);
            }
            break;
          }
          case 'list_plugins_ok': {
            const plugins: Record<string, PluginStatus> = {};
            for (const p of msg.plugins) plugins[p.name] = p;
            set({ plugins });
            break;
          }
          default:
            break;
        }
      },

      setParam(node, param, value) {
        const { patch } = get();
        if (patch) set({ patch: applyParam(patch, node, param, value), dirty: true });
        const type = findModule(patch, node)?.slot.type;
        pendingParams.set(`${node}:${param}`, type ? { node, param, value, type } : { node, param, value });
        scheduleFlush();
      },

      setZone(layer, zone) {
        const { patch } = get();
        if (patch) set({ patch: applyZone(patch, layer, zone), dirty: true });
        pendingZones.set(layer, { ...pendingZones.get(layer), ...zone });
        scheduleFlush();
      },

      flush() {
        flushScheduled = false;
        // offline / not yet synced: keep everything; the next `state` re-schedules the flush
        if (!synced) return;
        for (const [k, { node, param, value }] of pendingParams) {
          if (!client.send({ type: 'set_param', node, param, value })) return;
          pendingParams.delete(k);
        }
        for (const [layer, zone] of pendingZones) {
          if (!client.send({ type: 'set_zone', layer, zone })) return;
          pendingZones.delete(layer);
        }
        for (const [node, { bypass }] of pendingBypass) {
          if (!client.send({ type: 'set_fx_bypass', node, bypass })) return;
          pendingBypass.delete(node);
        }
        if (pendingTransport) {
          if (!client.send({ type: 'transport', ...pendingTransport })) return;
          pendingTransport = null;
        }
        if (pendingPattern) {
          if (client.status !== 'open') return;
          patternInFlight++;
          void client
            .request({ type: 'set_pattern', pattern: pendingPattern }, { expect: 'set_pattern_ok' })
            .catch(reportError)
            .finally(() => patternInFlight--);
          pendingPattern = null;
        }
      },

      connectionChanged(status) {
        if (status !== 'open') synced = false;
      },

      setBypass(node, bypass) {
        const { patch } = get();
        if (patch) set({ patch: applyBypass(patch, node, bypass), dirty: true });
        const type = findModule(patch, node)?.slot.type;
        pendingBypass.set(node, type ? { bypass, type } : { bypass });
        scheduleFlush();
      },

      selectLayer(layer) {
        const l = findLayer(get().patch, layer);
        set({ selectedLayer: layer, selectedModule: l?.instrument.node ?? get().selectedModule });
      },

      selectModule(node, layer) {
        set(layer !== undefined ? { selectedModule: node, selectedLayer: layer } : { selectedModule: node });
      },

      setTransport(t) {
        const upd: Partial<EngineState> = { transport: { ...get().transport, ...t } };
        // Meter changes resize the current pattern on the engine side; mirror that optimistically.
        // Swing lives in the pattern too; keep both in sync so a later set_pattern does not revert them.
        const cur = get().pattern;
        const apply = (p: Pattern): Pattern => {
          let q = p;
          if (t.time_sig) q = setTimeSig(q, t.time_sig[0], t.time_sig[1]);
          if (t.swing !== undefined) q = { ...q, swing: t.swing };
          return q;
        };
        if (cur && (t.time_sig || t.swing !== undefined)) {
          upd.pattern = { ...cur, edited: cur.edited || !!t.time_sig, pattern: apply(cur.pattern) };
          if (pendingPattern) pendingPattern = apply(pendingPattern);
        }
        set(upd);
        pendingTransport = { ...pendingTransport, ...t };
        scheduleFlush();
      },

      loadPattern(path) {
        pendingPattern = null; // unsent edits belonged to the previous pattern
        get().setTransport({ pattern: path });
        set({ pattern: null }); // no edits on the old pattern until the new one arrives
      },

      async refreshPatterns() {
        await structural(() => client.request({ type: 'list_patterns' }, { expect: 'list_patterns_ok' }));
      },

      fetchPattern() {
        if (patternFetch) return patternFetch;
        patternFetch = (async () => {
          try {
            await client.request({ type: 'get_pattern' }, { expect: 'get_pattern_ok' });
          } catch (e) {
            reportError(e);
          } finally {
            patternFetch = null;
          }
        })();
        return patternFetch;
      },

      editPattern(fn) {
        const cur = get().pattern;
        if (!cur) return;
        const next = fn(cur.pattern);
        if (next === cur.pattern) return;
        set({ pattern: { ...cur, edited: true, pattern: next } });
        pendingPattern = next;
        scheduleFlush();
      },

      async savePattern(name, overwrite) {
        try {
          get().flush();
          const r = await client.request(
            overwrite ? { type: 'save_pattern', name, overwrite: true } : { type: 'save_pattern', name },
            { expect: 'save_pattern_ok' },
          );
          const cur = get().pattern;
          if (cur) set({ pattern: { ...cur, path: r.path, edited: false, pattern: { ...cur.pattern, name } } });
          await get().refreshPatterns();
          return r.path;
        } catch (e) {
          reportError(e);
          return null;
        }
      },

      async loadPreset(path) {
        await structural(async () => {
          get().flush();
          await client.request({ type: 'load_preset', path }, { expect: 'load_preset_ok' });
          dropPatchEdits(); // anything unsent belonged to the previous patch
        });
      },

      async savePreset(name, category, overwrite) {
        try {
          get().flush();
          const msg: { type: 'save_preset'; name: string; category?: string; overwrite?: boolean } = {
            type: 'save_preset',
            name,
          };
          if (category) msg.category = category;
          if (overwrite) msg.overwrite = true;
          const r = await client.request(msg, { expect: 'save_preset_ok' });
          set({ dirty: false, presetPath: r.path });
          await get().refreshPresets();
          return r.path;
        } catch (e) {
          reportError(e);
          return null;
        }
      },

      async refreshPresets() {
        await structural(() => client.request({ type: 'list_presets' }, { expect: 'list_presets_ok' }));
      },

      addLayer: () => structural(() => client.request({ type: 'add_layer' }, { expect: 'state' })),
      removeLayer: (layer) => structural(() => client.request({ type: 'remove_layer', layer }, { expect: 'state' })),
      setInstrument: (layer, module) =>
        structural(() => client.request({ type: 'set_instrument', layer, module }, { expect: 'state' })),
      addFx: (layer, module, index) =>
        structural(() =>
          client.request(
            index === undefined ? { type: 'add_fx', layer, module } : { type: 'add_fx', layer, module, index },
            { expect: 'state' },
          ),
        ),
      removeFx: (node) => structural(() => client.request({ type: 'remove_fx', node }, { expect: 'state' })),
      async moveFx(node, to) {
        const { patch } = get();
        if (patch) set({ patch: applyMoveFx(patch, node, to) });
        await structural(() => client.request({ type: 'move_fx', node, to }, { expect: 'state' }));
      },

      note(on, note, velocity) {
        client.send({ type: 'note', on, note, velocity });
      },

      panic() {
        client.send({ type: 'panic' });
        clearNotes();
      },

      listDevices: () => structural(() => client.request({ type: 'list_devices' }, { expect: 'devices' })),
      setAudioDevice: (deviceType, name, sampleRate, bufferSize) =>
        structural(() => {
          const msg: {
            type: 'set_audio_device';
            device_type: string;
            name: string;
            sample_rate?: number;
            buffer_size?: number;
          } = { type: 'set_audio_device', device_type: deviceType, name };
          if (sampleRate) msg.sample_rate = sampleRate;
          if (bufferSize) msg.buffer_size = bufferSize;
          return client.request(msg, { expect: 'devices', timeoutMs: 15000 });
        }),
      rescanMidi: () => structural(() => client.request({ type: 'rescan_midi' }, { expect: 'devices' })),
      openAudioPanel: () => structural(() => client.request({ type: 'open_audio_panel' }, { expect: 'open_audio_panel_ok' })),
      restartAudio: () =>
        structural(() => client.request({ type: 'restart_audio' }, { expect: 'devices', timeoutMs: 15000 })),
      clearError: () => set({ lastError: null }),
      refreshPlugins: () => structural(() => client.request({ type: 'list_plugins' }, { expect: 'list_plugins_ok' })),
      reloadPlugin: (name) =>
        structural(() => client.request({ type: 'reload_plugin', name }, { expect: 'reload_plugin_ok' })),
      dismissPluginNotice: () => set({ pluginNotice: null }),
    };
  });

  return store;
}

/** Wire a store to its client: event dispatch, connection status, hello/bootstrap on (re)connect. */
export function bindClient(store: EngineStore, client: EngineClient): () => void {
  const offMsg = client.on('*', (m) => store.getState().handle(m as EngineMsg));
  const offStatus = client.onStatus((status, attempt) => {
    store.getState().connectionChanged(status);
    store.setState({ connection: { status, attempt, url: client.url } });
    if (status === 'open') {
      void bootstrap(client).catch((e) => store.setState({ lastError: String(e) }));
    } else if (status === 'reconnecting' || status === 'closed') {
      clearNotes();
    }
  });
  return () => {
    offMsg();
    offStatus();
  };
}

async function bootstrap(client: EngineClient): Promise<void> {
  await client.request({ type: 'hello', client: 'ui', version: PROTOCOL_VERSION }, { expect: 'state' });
  await Promise.all([
    client.request({ type: 'get_catalog' }, { expect: 'catalog_ok' }),
    client.request({ type: 'list_presets' }, { expect: 'list_presets_ok' }),
    client.request({ type: 'list_devices' }, { expect: 'devices' }),
    client.request({ type: 'list_patterns' }, { expect: 'list_patterns_ok' }).catch(() => undefined),
    // Optional: engines started with --no-plugins (or older mocks) may not answer.
    client.request({ type: 'list_plugins' }, { expect: 'list_plugins_ok' }).catch(() => undefined),
  ]);
}

export { MASTER_NODE };
