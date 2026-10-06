// Mock keysynth engine: implements docs/PROTOCOL.md over ws://127.0.0.1:7341 with a fake catalog, presets and
// telemetry, so the UI can be developed and tested without the C++ engine.
//
//   npm run mock               (port 7341)
//   npm run mock -- --demo     (also plays a slow chord loop so meters/keyboard move)
//   npm run mock -- --port 7400 --latency 20

import { WebSocketServer, WebSocket } from 'ws';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import type {
  AudioStatus,
  DevicesEvent,
  EngineMsg,
  FxSlot,
  MidiNote,
  ModuleSlot,
  Patch,
  PluginStatus,
  PresetEntry,
  TransportState,
  Zone,
} from '../src/protocol/types';
import { CATALOG, catalogMap, defaultParams } from './catalog';
import { INIT_PATCH, factoryPresets, makeFx, makeLayer, type MockPreset } from './presets';

export interface MockOptions {
  port?: number;
  host?: string;
  demo?: boolean;
  latencyMs?: number;
  telemetryHz?: number;
  quiet?: boolean;
}

export interface MockEngine {
  port: number;
  close(): Promise<void>;
  /** current patch (for tests) */
  readonly patch: Patch;
}

const ALLOWED_ORIGIN = /^http:\/\/(localhost|127\.0\.0\.1):(7341|5173|4173)$/;

const DEVICE_TYPES: { type: string; names: string[] }[] = [
  { type: 'ASIO', names: ['Focusrite USB ASIO', 'ASIO4ALL v2'] },
  { type: 'Windows Audio', names: ['Speakers (Realtek(R) Audio)', 'Headphones (Scarlett 2i2)'] },
  { type: 'Windows Audio (Exclusive Mode)', names: ['Speakers (Realtek(R) Audio)'] },
];

export function startMockEngine(opts: MockOptions = {}): Promise<MockEngine> {
  const host = opts.host ?? '127.0.0.1';
  const latency = opts.latencyMs ?? 0;
  const log = (...a: unknown[]) => {
    if (!opts.quiet) console.log('[mock]', ...a);
  };

  // ─────────────── model ───────────────
  let nextNode = 1;
  const presets: MockPreset[] = factoryPresets();
  let patch: Patch = assignNodes(INIT_PATCH());
  let presetPath: string | null = null;
  let dirty = false;
  let audio: AudioStatus = {
    type: 'ASIO',
    name: 'Focusrite USB ASIO',
    sampleRate: 48000,
    bufferSize: 64,
    inputLatencyMs: 1.6,
    outputLatencyMs: 2.2,
    running: true,
  };
  let midiInputs = ['Arturia KeyStep 37', 'loopMIDI Port'];
  // Plugin host simulation: `reload_plugin` goes compiling -> ok (or -> error for `broken_fx`).
  const plugins = new Map<string, PluginStatus>();
  const mockPlugin = (name: string, kind: PluginStatus['kind'], state: PluginStatus['state'], message = ''): PluginStatus => ({
    name,
    typeId: `plugin:${name}`,
    source: 'faust',
    state,
    message,
    kind,
    version: state === 'ok' ? 1 : 0,
    compileMs: 48,
    cached: false,
  });
  plugins.set('faust_pluck', mockPlugin('faust_pluck', 'instrument', 'ok'));
  plugins.set(
    'broken_fx',
    mockPlugin('broken_fx', '', 'error', 'broken_fx.dsp:3 : ERROR : syntax error, unexpected ENDDEF'),
  );
  const transport: TransportState = { playing: false, tempo: 120, metronome: false, metronome_volume: 0.5, drums: false };

  // live sim state
  const held = new Map<number, number>(); // note → velocity
  const energy = new Map<number, number>(); // layer node → linear level
  const midiQueue: MidiNote[] = [];
  let xruns = 0;
  let hornRpm = 40;

  function slot<T extends ModuleSlot>(s: T): T {
    return { ...s, node: s.node ?? nextNode++, params: { ...defaultParams(s.type), ...s.params } };
  }
  function assignNodes(p: Patch): Patch {
    const copy: Patch = structuredClone(p);
    for (const l of copy.layers) {
      l.node ??= nextNode++;
      l.instrument = slot(l.instrument);
      l.fx = l.fx.map((f) => slot(f));
    }
    copy.master.fx = copy.master.fx.map((f) => slot(f));
    return copy;
  }
  function stripNodes(p: Patch): Patch {
    const c: Patch = structuredClone(p);
    for (const l of c.layers) {
      delete l.node;
      delete l.instrument.node;
      for (const f of l.fx) delete f.node;
    }
    for (const f of c.master.fx) delete f.node;
    return c;
  }
  const allSlots = (): ModuleSlot[] => [
    ...patch.layers.flatMap((l) => [l.instrument, ...l.fx]),
    ...patch.master.fx,
  ];
  const findSlot = (node: number) => allSlots().find((s) => s.node === node);
  const findLayer = (node: number) => patch.layers.find((l) => l.node === node);
  const chainOf = (fxNode: number): FxSlot[] | undefined => {
    for (const l of patch.layers) if (l.fx.some((f) => f.node === fxNode)) return l.fx;
    if (patch.master.fx.some((f) => f.node === fxNode)) return patch.master.fx;
    return undefined;
  };

  const stateMsg = (id?: number) => ({
    type: 'state' as const,
    ...(id !== undefined ? { id } : {}),
    patch,
    presetPath,
    dirty,
    audio,
    transport,
  });
  const devicesMsg = (id?: number): DevicesEvent => ({
    type: 'devices',
    ...(id !== undefined ? { id } : {}),
    types: DEVICE_TYPES.map((d) => d.type),
    current: audio,
    available: DEVICE_TYPES,
    midiInputs,
  });
  const presetList = (): PresetEntry[] =>
    presets.map(({ path, name, category, tags, factory }) => ({ path, name, category, tags, factory }));

  // ─────────────── server ───────────────
  const wss = new WebSocketServer({
    host,
    port: opts.port ?? 7341,
    verifyClient: (info: { origin?: string }) => !info.origin || ALLOWED_ORIGIN.test(info.origin),
  });

  const send = (ws: WebSocket, msg: object) => {
    if (ws.readyState !== WebSocket.OPEN) return;
    const data = JSON.stringify(msg);
    if (latency > 0) setTimeout(() => ws.readyState === WebSocket.OPEN && ws.send(data), latency);
    else ws.send(data);
  };
  const broadcast = (msg: object, except?: WebSocket) => {
    for (const c of wss.clients) if (c !== except) send(c, msg);
  };
  const broadcastState = (replyTo?: WebSocket, id?: number) => {
    for (const c of wss.clients) send(c, c === replyTo ? stateMsg(id) : stateMsg());
  };
  const err = (ws: WebSocket, id: number | undefined, code: string, message: string) =>
    send(ws, { type: 'error', ...(id !== undefined ? { id } : {}), code, message });

  const structuralChange = (ws: WebSocket, id: number | undefined) => {
    dirty = true;
    broadcastState(ws, id);
  };

  function noteEvent(on: boolean, note: number, velocity: number) {
    if (on) held.set(note, velocity);
    else held.delete(note);
    midiQueue.push({ note, on, velocity: on ? velocity : 0 });
    if (on) {
      for (const l of patch.layers) {
        if (note >= l.zone.key_lo && note <= l.zone.key_hi && !l.zone.mute) {
          energy.set(l.node!, Math.min(1.4, (energy.get(l.node!) ?? 0) + (velocity / 127) * 0.7));
        }
      }
    }
  }

  function handle(ws: WebSocket, m: Record<string, unknown>) {
    const id = typeof m.id === 'number' ? m.id : undefined;
    const num = (k: string) => (typeof m[k] === 'number' ? (m[k] as number) : undefined);
    switch (m.type) {
      case 'hello':
        send(ws, stateMsg(id));
        return;
      case 'get_catalog':
        send(ws, { type: 'catalog_ok', id, modules: CATALOG });
        return;
      case 'set_param': {
        const node = num('node');
        const value = num('value');
        const param = typeof m.param === 'string' ? m.param : '';
        if (node === undefined || value === undefined || !param) return err(ws, id, 'bad_request', 'node, param, value required');
        if (node === 0 && param === 'volume_db') patch.master.volume_db = value;
        else {
          const s = findSlot(node);
          if (!s) return err(ws, id, 'unknown_node', `no node ${node}`);
          const spec = catalogMap[s.type]?.params.find((p) => p.id === param);
          if (!spec) return err(ws, id, 'unknown_param', `${s.type} has no param ${param}`);
          if (spec.flags & 1) return err(ws, id, 'read_only', `${param} is read-only`);
          s.params[param] = Math.min(spec.max, Math.max(spec.min, value));
        }
        dirty = true;
        broadcast({ type: 'param', node, param, value }, ws);
        return;
      }
      case 'load_preset': {
        const p = presets.find((x) => x.path === m.path);
        if (!p) return err(ws, id, 'not_found', `preset not found: ${String(m.path)}`);
        patch = assignNodes(p.patch);
        presetPath = p.path;
        dirty = false;
        if (patch.tempo) transport.tempo = patch.tempo;
        energy.clear();
        send(ws, { type: 'load_preset_ok', id });
        broadcastState();
        log('loaded', p.path);
        return;
      }
      case 'save_preset': {
        const name = typeof m.name === 'string' ? m.name.trim() : '';
        if (!name) return err(ws, id, 'bad_request', 'name required');
        const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
        const path = `userdata/presets/${slug}.json`;
        const existing = presets.findIndex((x) => x.path === path);
        if (existing >= 0 && !m.overwrite) return err(ws, id, 'exists', `preset "${name}" already exists`);
        const category = typeof m.category === 'string' && m.category ? m.category : patch.meta.category ?? 'User';
        patch.meta = { ...patch.meta, name, category };
        const entry: MockPreset = { path, name, category, tags: patch.meta.tags ?? [], factory: false, patch: stripNodes(patch) };
        if (existing >= 0) presets[existing] = entry;
        else presets.push(entry);
        presetPath = path;
        dirty = false;
        send(ws, { type: 'save_preset_ok', id, path });
        broadcastState();
        return;
      }
      case 'list_presets':
        send(ws, { type: 'list_presets_ok', id, presets: presetList() });
        return;
      case 'get_patch':
        send(ws, stateMsg(id));
        return;
      case 'set_patch': {
        if (!m.patch || typeof m.patch !== 'object') return err(ws, id, 'bad_request', 'patch required');
        patch = assignNodes(m.patch as Patch);
        return structuralChange(ws, id);
      }
      case 'add_layer': {
        const l = makeLayer(`Layer ${patch.layers.length + 1}`, 'va', {});
        const withNodes = assignNodes({ ...patch, layers: [l], master: { volume_db: 0, fx: [] } }).layers[0]!;
        const idx = num('index') ?? patch.layers.length;
        patch.layers.splice(Math.max(0, Math.min(idx, patch.layers.length)), 0, withNodes);
        return structuralChange(ws, id);
      }
      case 'remove_layer': {
        const i = patch.layers.findIndex((l) => l.node === m.layer);
        if (i < 0) return err(ws, id, 'unknown_node', 'no such layer');
        if (patch.layers.length === 1) return err(ws, id, 'invalid', 'cannot remove the last layer');
        patch.layers.splice(i, 1);
        return structuralChange(ws, id);
      }
      case 'set_zone': {
        const l = findLayer(num('layer') ?? -1);
        if (!l || !m.zone || typeof m.zone !== 'object') return err(ws, id, 'bad_request', 'layer and zone required');
        l.zone = { ...l.zone, ...(m.zone as Partial<Zone>) };
        dirty = true;
        return;
      }
      case 'set_instrument': {
        const l = findLayer(num('layer') ?? -1);
        const mod = typeof m.module === 'string' ? catalogMap[m.module] : undefined;
        if (!l || !mod || mod.kind !== 'instrument') return err(ws, id, 'bad_request', 'layer and instrument module required');
        l.instrument = { node: nextNode++, type: mod.typeId, params: defaultParams(mod.typeId), state: {} };
        return structuralChange(ws, id);
      }
      case 'add_fx': {
        const layerId = num('layer');
        const mod = typeof m.module === 'string' ? catalogMap[m.module] : undefined;
        const chain = layerId === 0 ? patch.master.fx : findLayer(layerId ?? -1)?.fx;
        if (!chain || !mod || mod.kind !== 'effect') return err(ws, id, 'bad_request', 'layer and effect module required');
        const f = { ...makeFx(mod.typeId), node: nextNode++ };
        const idx = num('index') ?? chain.length;
        chain.splice(Math.max(0, Math.min(idx, chain.length)), 0, f);
        return structuralChange(ws, id);
      }
      case 'remove_fx': {
        const node = num('node') ?? -1;
        const chain = chainOf(node);
        if (!chain) return err(ws, id, 'unknown_node', 'no such fx');
        chain.splice(chain.findIndex((f) => f.node === node), 1);
        return structuralChange(ws, id);
      }
      case 'move_fx': {
        const node = num('node') ?? -1;
        const to = num('to');
        const chain = chainOf(node);
        if (!chain || to === undefined) return err(ws, id, 'bad_request', 'node and to required');
        const from = chain.findIndex((f) => f.node === node);
        const [item] = chain.splice(from, 1);
        chain.splice(Math.max(0, Math.min(to, chain.length)), 0, item!);
        return structuralChange(ws, id);
      }
      case 'set_fx_bypass': {
        const node = num('node') ?? -1;
        const f = chainOf(node)?.find((x) => x.node === node);
        if (!f) return err(ws, id, 'unknown_node', 'no such fx');
        f.bypass = !!m.bypass;
        dirty = true;
        return;
      }
      case 'rescan_midi':
        midiInputs = Math.random() < 0.5 ? ['Arturia KeyStep 37', 'loopMIDI Port'] : ['Arturia KeyStep 37', 'loopMIDI Port', 'Roland A-49'];
        send(ws, devicesMsg(id));
        return;
      case 'note': {
        const note = num('note');
        if (note === undefined || note < 0 || note > 127) return err(ws, id, 'bad_request', 'note 0..127 required');
        noteEvent(!!m.on, note, Math.max(1, Math.min(127, num('velocity') ?? 100)));
        return;
      }
      case 'cc':
        return;
      case 'transport': {
        for (const k of ['playing', 'tempo', 'metronome', 'metronome_volume', 'pattern', 'drums'] as const) {
          if (k in m) (transport as unknown as Record<string, unknown>)[k] = m[k];
        }
        transport.tempo = Math.min(300, Math.max(20, transport.tempo));
        broadcast(stateMsg(), ws);
        return;
      }
      case 'list_devices':
        send(ws, devicesMsg(id));
        return;
      case 'set_audio_device': {
        const t = DEVICE_TYPES.find((d) => d.type === m.device_type);
        const name = typeof m.name === 'string' ? m.name : '';
        if (!t || !t.names.includes(name)) return err(ws, id, 'not_found', 'unknown device');
        const sr = num('sample_rate') ?? audio.sampleRate;
        const bs = num('buffer_size') ?? audio.bufferSize;
        const isAsio = t.type === 'ASIO';
        audio = {
          type: t.type,
          name,
          sampleRate: sr,
          bufferSize: bs,
          inputLatencyMs: +((bs / sr) * 1000 + (isAsio ? 0.3 : 5)).toFixed(2),
          outputLatencyMs: +((bs / sr) * 1000 + (isAsio ? 0.9 : 10)).toFixed(2),
          running: true,
        };
        for (const c of wss.clients) send(c, c === ws ? devicesMsg(id) : devicesMsg());
        broadcastState();
        return;
      }
      case 'panic':
        for (const n of [...held.keys()]) noteEvent(false, n, 0);
        energy.clear();
        return;
      case 'list_plugins':
        send(ws, {
          type: 'list_plugins_ok',
          id,
          plugins: [...plugins.values()],
          faust: { available: true, version: '2.88.0 (mock)', reason: '' },
        });
        return;
      case 'reload_plugin': {
        const name = typeof m.name === 'string' ? m.name : '';
        const cur = plugins.get(name);
        if (!/^[a-z][a-z0-9_]{0,63}$/.test(name) || !cur) return err(ws, id, 'not_found', `unknown plugin '${name}'`);
        send(ws, { type: 'reload_plugin_ok', id, name });
        const compiling = { ...cur, state: 'compiling' as const, message: '' };
        plugins.set(name, compiling);
        broadcast({ type: 'plugin_status', plugin: compiling });
        setTimeout(() => {
          const done =
            name === 'broken_fx'
              ? { ...cur, state: 'error' as const }
              : { ...cur, state: 'ok' as const, message: '', version: cur.version + 1, compileMs: 52 };
          plugins.set(name, done);
          broadcast({ type: 'plugin_status', plugin: done });
        }, 300);
        return;
      }
      default:
        return err(ws, id, 'unknown_type', `unknown message type: ${String(m.type)}`);
    }
  }

  wss.on('connection', (ws) => {
    log('client connected');
    ws.on('message', (raw) => {
      let m: unknown;
      try {
        m = JSON.parse(String(raw));
      } catch {
        return err(ws, undefined, 'bad_json', 'invalid JSON');
      }
      if (!m || typeof m !== 'object' || typeof (m as { type?: unknown }).type !== 'string') {
        return err(ws, undefined, 'bad_request', 'missing type');
      }
      try {
        handle(ws, m as Record<string, unknown>);
      } catch (e) {
        err(ws, (m as { id?: number }).id, 'internal', String(e));
      }
    });
  });

  // ─────────────── telemetry & midi ───────────────
  const hz = opts.telemetryHz ?? 30;
  const toDb = (x: number) => (x <= 1e-5 ? -120 : 20 * Math.log10(x));
  const telemetryTimer = setInterval(() => {
    if (midiQueue.length) broadcast({ type: 'midi', notes: midiQueue.splice(0) });
    const layers: Record<string, [number, number]> = {};
    let masterLin = 0;
    const anySolo = patch.layers.some((l) => l.zone.solo);
    for (const l of patch.layers) {
      const n = l.node!;
      const heldInZone = [...held.keys()].filter((k) => k >= l.zone.key_lo && k <= l.zone.key_hi).length;
      const audible = !l.zone.mute && (!anySolo || l.zone.solo);
      let e = energy.get(n) ?? 0;
      const sustainLevel = audible && heldInZone ? 0.25 + 0.06 * heldInZone : 0;
      e = Math.max(sustainLevel, e * (heldInZone ? 0.96 : 0.86));
      energy.set(n, e);
      const g = audible ? e * Math.pow(10, l.zone.volume_db / 20) : 0;
      const wob = 1 + (Math.random() - 0.5) * 0.12;
      const pan = l.zone.pan;
      layers[String(n)] = [toDb(g * wob * Math.min(1, 1 - pan)), toDb(g * wob * Math.min(1, 1 + pan))];
      masterLin += g;
    }
    masterLin *= Math.pow(10, patch.master.volume_db / 20);
    const readouts: Record<string, Record<string, number>> = {};
    for (const l of patch.layers) {
      const e = energy.get(l.node!) ?? 0;
      if (l.instrument.type === 'va') readouts[String(l.instrument.node)] = { active_voices: Math.min(16, held.size) };
      for (const f of [...l.fx, ...patch.master.fx]) {
        if (f.type === 'compressor') {
          const over = toDb(e) - (f.params.threshold ?? -18);
          readouts[String(f.node)] = { gr: f.bypass ? 0 : -Math.max(0, Math.min(30, over * (1 - 1 / (f.params.ratio ?? 4)))) };
        }
        if (f.type === 'rotary') {
          const target = f.params.speed ? 340 : 40;
          hornRpm += (target - hornRpm) * 0.04;
          readouts[String(f.node)] = { horn_rpm: hornRpm };
        }
      }
    }
    const voices = held.size * Math.max(1, patch.layers.length);
    if (Math.random() < 0.0008) xruns++;
    broadcast({
      type: 'telemetry',
      cpu: Math.min(1, 0.04 + voices * 0.012 + Math.random() * 0.02),
      xruns,
      voices,
      meters: { master: [toDb(masterLin * 0.98), toDb(masterLin)], layers },
      readouts,
    } satisfies EngineMsg);
  }, 1000 / hz);

  // demo: slow chord loop
  let demoTimer: ReturnType<typeof setInterval> | null = null;
  if (opts.demo) {
    const chords = [
      [45, 57, 60, 64],
      [41, 57, 60, 65],
      [48, 55, 60, 64],
      [43, 55, 59, 62],
    ];
    let i = 0;
    demoTimer = setInterval(() => {
      const prev = chords[(i + chords.length - 1) % chords.length]!;
      for (const n of prev) noteEvent(false, n, 0);
      for (const n of chords[i % chords.length]!) noteEvent(true, n, 90);
      i++;
    }, 1600);
  }

  return new Promise((resolve, reject) => {
    wss.once('error', reject);
    wss.once('listening', () => {
      const addr = wss.address();
      const port = typeof addr === 'object' && addr ? addr.port : (opts.port ?? 7341);
      log(`listening on ws://${host}:${port}${opts.demo ? ' (demo)' : ''}`);
      resolve({
        port,
        get patch() {
          return patch;
        },
        close: () =>
          new Promise<void>((res) => {
            clearInterval(telemetryTimer);
            if (demoTimer) clearInterval(demoTimer);
            for (const c of wss.clients) c.terminate();
            wss.close(() => res());
          }),
      });
    });
  });
}

// CLI
const isMain =
  !!process.argv[1] && path.resolve(fileURLToPath(import.meta.url)).toLowerCase() === path.resolve(process.argv[1]).toLowerCase();
if (isMain) {
  const args = process.argv.slice(2);
  const val = (k: string) => {
    const i = args.indexOf(k);
    return i >= 0 ? args[i + 1] : undefined;
  };
  startMockEngine({
    port: Number(val('--port') ?? 7341),
    demo: args.includes('--demo'),
    latencyMs: Number(val('--latency') ?? 0),
  }).catch((e) => {
    console.error('[mock] failed to start:', e);
    process.exit(1);
  });
}
