// Mock keysynth engine: implements docs/PROTOCOL.md over ws://127.0.0.1:7441 (NOT the engine's 7341, so a leftover
// mock can never take the real engine's port) with a fake catalog, presets and telemetry; hello reports
// `engine.kind: "mock"`, so production pages refuse it (src/platform/identity.ts). UI without the C++ engine:
//
//   npm run mock               (port 7441; then `npm run dev:mock` = Vite pointed at it)
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
  Pattern,
  PatternSnapshot,
  PluginStatus,
  PresetEntry,
  ControlKeys,
  Song,
  SongInfo,
  TransportState,
  Zone,
} from '../src/protocol/types';
import { CATALOG, catalogMap, defaultParams } from './catalog';
import { INIT_PATCH, factoryPresets, makeFx, makeLayer, type MockPreset } from './presets';
import { factorySongs, type MockSong } from './songs';
import { factoryPatterns, parsePatternJson, type MockPattern } from './patterns';
import { emptyPattern, numSteps, setTimeSig } from '../src/lib/stepGrid';

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

const ALLOWED_ORIGIN = /^http:\/\/(localhost|127\.0\.0\.1):(7340|7341|7440|7441|5173|4173)$/;
/** PROTOCOL.md "Sessions, Origin, Host": the platform page gets a `web` session (default-deny allow-list). */
const WEB_ORIGIN = 'https://mvv42.ru';
const WEB_ALLOWED = new Set([
  'hello', 'get_catalog', 'set_param', 'get_patch', 'list_presets', 'add_layer', 'remove_layer', 'set_zone',
  'set_instrument', 'add_fx', 'remove_fx', 'move_fx', 'set_fx_bypass', 'note', 'cc', 'panic', 'list_patterns',
  'set_pattern', 'rescan_midi', 'list_devices', 'set_audio_device', 'open_audio_panel', 'restart_audio', 'list_plugins',
  'save_preset', 'save_pattern', 'load_preset', 'get_pattern', 'transport', 'set_patch', 'cloud_link', 'cloud_unlink',
  'sync_now', 'samples_list', 'samples_fetch', 'samples_cancel', 'samples_delete', 'chord_learn', 'song_list',
  'song_get', 'song_save', 'song_delete', 'song_select', 'song_section', 'song_next', 'song_prev', 'song_close',
  'song_control_keys',
]);
/** Remote (relay) sessions: web + these denied (docs/specs/2026-10-12-remote.md §10.3). */
const REMOTE_DENIED = new Set(['cloud_link', 'cloud_unlink', 'set_audio_device', 'open_audio_panel', 'samples_delete', 'reload_plugin']);
/** Set by mock/relay-mock.ts on its per-controller connections: the session gets trust `remote`. */
export const RELAY_HEADER = 'x-keysynth-relay';
const MOCK_VERSION = '0.1.0';
/** Default mock port (the real engine: 7341). */
export const MOCK_PORT = 7441;
/** Mock cloud: any `ksd_` + 43 base64url token links; tokens containing "BAD" are rejected, "OTHER" = account 42. */
const TOKEN_RE = /^ksd_[A-Za-z0-9_-]{43}$/;

const DEVICE_TYPES: { type: string; names: string[] }[] = [
  // long and Cyrillic names on purpose (layout of the audio dialog)
  { type: 'ASIO', names: ['Focusrite USB ASIO', 'ASIO4ALL v2', 'Yamaha Steinberg USB ASIO (UR22mkII, 64-bit driver 2.1.4)'] },
  { type: 'Windows Audio', names: ['Speakers (Realtek(R) Audio)', 'Headphones (Scarlett 2i2)', 'Динамики (Устройство с поддержкой High Definition Audio)'] },
  { type: 'Windows Audio (Exclusive Mode)', names: ['Speakers (Realtek(R) Audio)'] },
];
/** The mock ASIO driver only accepts these; another request falls back to its preferred size, like JUCE does. */
const ASIO_BUFFER_SIZES = [64, 128, 256];
const ASIO_PREFERRED_BUFFER = 256;

export function startMockEngine(opts: MockOptions = {}): Promise<MockEngine> {
  const host = opts.host ?? '127.0.0.1';
  const latency = opts.latencyMs ?? 0;
  const log = (...a: unknown[]) => {
    if (!opts.quiet) console.log('[mock]', ...a);
  };

  // ─────────────── model ───────────────
  // rhythm (drum sequencer) model
  const patterns: MockPattern[] = factoryPatterns();
  const defaultPattern = patterns.find((p) => p.entry.path.endsWith('/basic-rock.json'));
  const current: PatternSnapshot = defaultPattern
    ? { path: defaultPattern.entry.path, edited: false, pattern: structuredClone(defaultPattern.pattern) }
    : { path: '', edited: false, pattern: emptyPattern() };
  const transport: TransportState = {
    playing: false,
    tempo: 120,
    metronome: false,
    metronome_volume: 0.5,
    pattern: current.path,
    pattern_edited: false,
    drums: true,
    drums_volume: 0.8,
    swing: current.pattern.swing,
    time_sig: [...current.pattern.time_sig],
    count_in: false,
  };
  let playStartedAt = 0;
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
    hasControlPanel: true,
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
      if (l.midi?.length) l.midi = l.midi.map((f) => slot(f));
      else delete l.midi;
    }
    copy.master.fx = copy.master.fx.map((f) => slot(f));
    const r = copy.rhythm ?? {};
    copy.rhythm = { node: r.node ?? nextNode++, kit: { ...defaultParams('drums'), ...r.kit }, pattern: r.pattern ?? transport.pattern ?? '' };
    return copy;
  }
  function stripNodes(p: Patch): Patch {
    const c: Patch = structuredClone(p);
    for (const l of c.layers) {
      delete l.node;
      delete l.instrument.node;
      for (const f of l.fx) delete f.node;
      for (const f of l.midi ?? []) delete f.node;
    }
    for (const f of c.master.fx) delete f.node;
    if (c.rhythm) delete c.rhythm.node;
    return c;
  }
  const allSlots = (): ModuleSlot[] => [
    ...patch.layers.flatMap((l) => [...(l.midi ?? []), l.instrument, ...l.fx]),
    ...patch.master.fx,
    ...(patch.rhythm?.node !== undefined ? [{ node: patch.rhythm.node, type: 'drums', params: (patch.rhythm.kit ??= {}) }] : []),
  ];
  const findSlot = (node: number) => allSlots().find((s) => s.node === node);
  const findLayer = (node: number) => patch.layers.find((l) => l.node === node);
  const chainOf = (fxNode: number): FxSlot[] | undefined => {
    for (const l of patch.layers) {
      if (l.fx.some((f) => f.node === fxNode)) return l.fx;
      if (l.midi?.some((f) => f.node === fxNode)) return l.midi;
    }
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
    // Like the engine: sizes/rates only for the open device's type (others are unknown until opened).
    available: DEVICE_TYPES.map((d) =>
      d.type === audio.type
        ? { ...d, sampleRates: [44100, 48000, 96000], bufferSizes: d.type === 'ASIO' ? ASIO_BUFFER_SIZES : [128, 256, 512, 1024] }
        : { ...d, sampleRates: [], bufferSizes: [] },
    ),
    midiInputs,
  });
  const presetList = (): PresetEntry[] =>
    presets.map(({ path, name, category, tags, factory }) => ({ path, name, category, tags, factory }));

  // ─────────────── cloud & samples (PROTOCOL.md "Cloud & samples") ───────────────
  type MockEntry = { id: string; name: string; sfz: string; bytes: number; cachedBytes: number; local?: boolean };
  type MockLib = { id: string; name: string; group: string; licence: string; author: string; attribution: string; entries: MockEntry[] };
  type MockJob = { id: string; scope: string; target: string; state: 'queued' | 'running'; bytesDone: number; bytesTotal: number;
    filesDone: number; filesTotal: number; rate: number; entries: string[]; auto: boolean };
  const MB = 1024 * 1024;
  const entry = (lib: string, sfz: string, mb: number, local = false): MockEntry =>
    ({ id: `${lib}/${sfz}`, name: sfz.replace(/^.*\//, '').replace(/\.sfz$/, ''), sfz: `assets/samples/${lib}/${sfz}`,
       bytes: mb * MB, cachedBytes: 0, local });
  const groups = [{ id: 'piano', name: 'Piano' }, { id: 'epiano', name: 'Electric piano' }];
  const libs: MockLib[] = [
    { id: 'salamander', name: 'Salamander Grand Piano V3', group: 'piano', licence: 'CC-BY-3.0', author: 'Alexander Holm',
      attribution: 'Salamander Grand Piano by Alexander Holm, CC BY 3.0.',
      entries: [entry('salamander', 'SalamanderGrandPiano-V3.sfz', 40)] },
    { id: 'jrhodes3c', name: 'jRhodes3c', group: 'epiano', licence: 'CC0', author: 'jlearman', attribution: 'jRhodes3c (CC0)',
      entries: [entry('jrhodes3c', 'jRhodes3c-looped.sfz', 12), entry('jrhodes3c', 'jRhodes3c-stereo.sfz', 18)] },
    { id: 'localdemo', name: 'Local demo', group: 'epiano', licence: 'CC0', author: 'you', attribution: '',
      entries: [entry('localdemo', 'demo.sfz', 1, true)] },
  ];
  const deviceId = '3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40';
  let link: { accountId: string; accountName: string; expiresAt: string } | null = null;
  const seenAccounts = new Set<string>();
  const jobs: MockJob[] = [];
  let lastSyncAt: string | null = null;
  let syncState: 'disabled' | 'idle' | 'syncing' | 'offline' | 'error' = 'disabled';
  const trustOf = new WeakMap<WebSocket, 'local' | 'web' | 'remote'>();
  const cloudStatus = () => ({
    linked: !!link, accountId: link?.accountId ?? null, accountName: link?.accountName ?? null, deviceId,
    expiresAt: link?.expiresAt ?? null, online: true, error: null as string | null,
  });
  const syncStatus = () => ({ type: 'sync_status', state: syncState, lastSyncAt, pending: 0, conflicts: [], error: null });
  const allEntries = () => libs.flatMap((l) => l.entries);
  const entryStatus = (e: MockEntry) => (e.local ? 'local' : e.cachedBytes >= e.bytes ? 'cached' : e.cachedBytes > 0 ? 'partial' : 'missing');
  const jobFor = (lib: MockLib, e?: MockEntry) =>
    (e && jobs.find((j) => j.id === `entry:${e.id}`)) || jobs.find((j) => j.id === `library:${lib.id}`) || jobs.find((j) => j.id === 'all');
  const samplesList = (id?: number) => ({
    type: 'samples_list_ok', ...(id !== undefined ? { id } : {}),
    manifest: link ? { state: 'ok', etag: '"9c1f0a2b3c4d5e6f"', fetchedAt: new Date().toISOString() } : { state: 'none', etag: null, fetchedAt: null },
    cacheBytes: allEntries().reduce((s, e) => s + (e.local ? 0 : e.cachedBytes), 0),
    freeBytes: 98765432100,
    groups: link
      ? groups.map((g) => ({
          ...g,
          libraries: libs.filter((l) => l.group === g.id).map((l) => {
            const st = l.entries.map(entryStatus);
            return {
              id: l.id, name: l.name, licence: l.licence, author: l.author, attribution: l.attribution,
              bytes: l.entries.reduce((s, e) => s + e.bytes, 0), cachedBytes: l.entries.reduce((s, e) => s + e.cachedBytes, 0),
              status: st.includes('local') ? 'local' : st.every((s) => s === 'cached') ? 'cached' : st.every((s) => s === 'missing') ? 'missing' : 'partial',
              job: jobs.find((j) => j.id === `library:${l.id}` || j.id === 'all')?.id ?? null,
              entries: l.entries.map((e) => ({ id: e.id, name: e.name, sfz: e.sfz, bytes: e.bytes, cachedBytes: e.cachedBytes,
                status: entryStatus(e), job: jobFor(l, e)?.id ?? null })),
            };
          }),
        }))
      : [],
  });
  const targetEntries = (scope: string, target: string) =>
    scope === 'all' ? allEntries() : scope === 'library' ? (libs.find((l) => l.id === target)?.entries ?? []) : allEntries().filter((e) => e.id === target);
  const startJob = (scope: string, target: string, auto = false): { job: string; existing: boolean } | null => {
    const id = scope === 'all' ? 'all' : `${scope}:${target}`;
    if (jobs.some((j) => j.id === id)) return { job: id, existing: true };
    const es = targetEntries(scope, target);
    if (!es.length) return null;
    const todo = es.filter((e) => !e.local && e.cachedBytes < e.bytes);
    const job: MockJob = { id, scope, target: scope === 'all' ? '' : target, state: 'queued',
      bytesDone: 0, bytesTotal: todo.reduce((s, e) => s + e.bytes - e.cachedBytes, 0), filesDone: 0,
      filesTotal: todo.length * 30, rate: 0, entries: es.map((e) => e.id), auto };
    if (auto) jobs.unshift(job);
    else jobs.push(job);
    return { job: id, existing: false };
  };
  const endJob = (j: MockJob, cancelled: boolean) => {
    jobs.splice(jobs.indexOf(j), 1);
    const cached = j.entries.filter((id) => { const e = allEntries().find((x) => x.id === id); return e && entryStatus(e) === 'cached'; });
    broadcast({ type: 'samples_done', job: j.id, scope: j.scope, target: j.target, cancelled, entries: cached });
  };
  // Download sim: 2 "parallel files", ~6 MB/s per job; tokens are fine, so only `samples_cancel` stops jobs.
  const samplesTimer = setInterval(() => {
    if (!jobs.length) return;
    for (const j of jobs.slice(0, 2)) {
      j.state = 'running';
      const step = 1.2 * MB;
      let left = step;
      for (const id of j.entries) {
        const e = allEntries().find((x) => x.id === id);
        if (!e || e.local || left <= 0) continue;
        const take = Math.min(left, e.bytes - e.cachedBytes);
        e.cachedBytes += take;
        left -= take;
      }
      j.bytesDone = Math.min(j.bytesTotal, j.bytesDone + step - left);
      j.filesDone = j.bytesTotal ? Math.floor((j.filesTotal * j.bytesDone) / j.bytesTotal) : j.filesTotal;
      j.rate = 6 * MB;
    }
    broadcast({ type: 'samples_progress', jobs: jobs.map(({ entries, auto, ...pub }) => (void entries, void auto, pub)) });
    for (const j of [...jobs]) if (j.bytesDone >= j.bytesTotal) endJob(j, false);
  }, 200);
  /** Auto-fetch: a sampler sfz under assets/samples/ that is not cached starts an entry job at the queue front. */
  const autoFetch = () => {
    if (!link) return;
    for (const l of patch.layers) {
      const sfz = l.instrument.type === 'sampler' ? (l.instrument.state as { sfz?: string } | undefined)?.sfz : undefined;
      const e = sfz ? allEntries().find((x) => x.sfz === sfz) : undefined;
      if (e && entryStatus(e) !== 'cached' && entryStatus(e) !== 'local') startJob('entry', e.id, true);
    }
  };
  const runSync = () => {
    if (!link) return;
    syncState = 'syncing';
    broadcast(syncStatus());
    setTimeout(() => {
      if (!link) return;
      syncState = 'idle';
      lastSyncAt = new Date().toISOString().replace(/\.\d+Z$/, 'Z');
      broadcast(syncStatus());
    }, 400);
  };
  const isLogical = (p: unknown, prefixes: string[]) =>
    // eslint-disable-next-line no-control-regex -- control chars are exactly what the path rule rejects
    typeof p === 'string' && !/(^|\/)\.\.?(\/|$)|\\|:|^\/|\/\/|[\x00-\x1f]/.test(p) && prefixes.some((x) => p.startsWith(x) && p.length > x.length);
  const webForbidden = (m: Record<string, unknown>): string | null => {
    const t = String(m.type);
    if (!WEB_ALLOWED.has(t)) return `'${t}' is not available to web sessions`;
    if (t === 'load_preset' && !isLogical(m.path, ['presets/factory/', 'userdata/presets/'])) return 'load_preset: bad path';
    if ((t === 'song_get' || t === 'song_select' || t === 'song_delete') && !isLogical(m.path, ['presets/songs/', 'userdata/songs/']))
      return `${t}: bad path`;
    const pat = (v: unknown) => v === undefined || v === '' || isLogical(v, ['presets/patterns/', 'userdata/patterns/']);
    if (t === 'get_pattern' && !pat(m.path)) return 'get_pattern: bad path';
    if (t === 'transport' && !pat(m.pattern)) return 'transport: bad pattern';
    if (t === 'set_patch') {
      const p = m.patch as Patch | undefined;
      for (const l of p?.layers ?? []) {
        const sfz = (l.instrument?.state as { sfz?: string } | undefined)?.sfz;
        if (sfz && !isLogical(sfz, ['assets/samples/'])) return 'set_patch: sampler sfz must be under assets/samples/';
      }
      if (p?.rhythm && !pat((p.rhythm as { pattern?: string }).pattern)) return 'set_patch: bad rhythm.pattern';
    }
    return null;
  };

  // ─────────────── songs (PROTOCOL.md "Songs") ───────────────
  const songs: MockSong[] = factorySongs();
  let globalKeys: ControlKeys = { enabled: false, prev: 21, next: 22 };
  let active: { path: string; song: Song; section: number } | null = null;
  const SONG_SLUG = /^[a-z0-9]+(-[a-z0-9]+)*$/;
  const songSlug = (s: string) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 64).replace(/-+$/, '');
  const songInfo = (s: MockSong): SongInfo => ({
    path: s.path, id: s.song.id, title: s.song.title, artist: s.song.artist ?? '', sections: s.song.sections.length, factory: s.factory,
  });
  const songList = () =>
    [...songs].sort((a, b) => (a.factory === b.factory ? a.song.title.localeCompare(b.song.title) : a.factory ? -1 : 1)).map(songInfo);
  const effectiveKeys = () =>
    active?.song.control_keys ? { ...active.song.control_keys, source: 'song' as const } : { ...globalKeys, source: 'global' as const };
  const songState = () => ({
    type: 'song_state', path: active?.path ?? null, song: active?.song ?? null, section: active ? active.section : -1,
    controlKeys: effectiveKeys(), globalControlKeys: globalKeys,
  });
  const keyOk = (v: unknown) => v === null || (typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= 127);
  const tempoOk = (v: unknown) => v === undefined || (typeof v === 'number' && v >= 20 && v <= 400);
  const textOk = (v: unknown, max: number) => v === undefined || (typeof v === 'string' && v.length <= max);
  /** song_save validation: the message names the field (PROTOCOL.md). */
  const songProblem = (s: Record<string, unknown>): string | null => {
    if (typeof s.title !== 'string' || !s.title.trim() || s.title.length > 200) return 'title: 1..200 chars';
    if (!textOk(s.artist, 200) || !textOk(s.key, 200)) return 'artist/key: string';
    if (!textOk(s.notes, 2000)) return 'notes: ≤ 2000 chars';
    if (!tempoOk(s.tempo)) return 'tempo: 20..400';
    if (s.time_sig !== undefined) {
      const ts = s.time_sig as unknown[];
      if (!Array.isArray(ts) || ts.length !== 2 || !((ts[0] as number) >= 1 && (ts[0] as number) <= 16) || ![2, 4, 8, 16].includes(ts[1] as number))
        return 'time_sig: [1..16, 2|4|8|16]';
    }
    if (s.control_keys !== undefined) {
      const ck = s.control_keys as Record<string, unknown>;
      if (!ck || typeof ck !== 'object' || typeof ck.enabled !== 'boolean' || !keyOk(ck.prev) || !keyOk(ck.next)) return 'control_keys';
    }
    const secs = s.sections;
    if (!Array.isArray(secs) || secs.length < 1 || secs.length > 64) return 'sections: 1..64 entries';
    for (const [i, x] of (secs as Record<string, unknown>[]).entries()) {
      if (!x || typeof x !== 'object' || typeof x.name !== 'string' || !x.name.trim() || x.name.length > 60) return `sections[${i}].name: 1..60 chars`;
      if (x.preset !== undefined && !(isLogical(x.preset, ['presets/factory/', 'userdata/presets/']) || SONG_SLUG.test(String(x.preset))))
        return `sections[${i}].preset: logical path or slug`;
      if (x.pattern !== undefined && x.pattern !== '' && !isLogical(x.pattern, ['presets/patterns/', 'userdata/patterns/']))
        return `sections[${i}].pattern: logical path`;
      if (x.drums !== undefined && typeof x.drums !== 'boolean') return `sections[${i}].drums: bool`;
      if (!tempoOk(x.tempo)) return `sections[${i}].tempo: 20..400`;
      if (!textOk(x.notes, 2000)) return `sections[${i}].notes: ≤ 2000 chars`;
    }
    return null;
  };
  const closeSong = () => {
    if (!active) return;
    active = null;
    broadcast(songState());
  };
  /** Apply the active section: preset → song time_sig → pattern → tempo → drums (ARCHITECTURE §18). */
  const applySection = (): string[] => {
    const warnings: string[] = [];
    if (!active) return warnings;
    const { song } = active;
    const sec = song.sections[active.section];
    if (!sec) return warnings;
    const fail = (w: string) => {
      warnings.push(w);
      broadcast({ type: 'log', level: 'error', notify: true, message: w });
    };
    if (sec.preset !== undefined) {
      const path = sec.preset.includes('/') ? sec.preset : `userdata/presets/${sec.preset}.json`;
      const p = presets.find((x) => x.path === path);
      if (p) applyPreset(p);
      else fail(`song: preset not found: ${sec.preset}`);
    }
    if (song.time_sig) {
      current.pattern = setTimeSig(current.pattern, song.time_sig[0], song.time_sig[1]);
      transport.time_sig = [...song.time_sig];
    }
    if (sec.pattern !== undefined && !applyPattern(sec.pattern)) fail(`song: pattern not found: ${sec.pattern}`);
    const tempo = sec.tempo ?? song.tempo;
    if (tempo !== undefined) transport.tempo = tempo;
    if (sec.drums === true) {
      if (!transport.playing) playStartedAt = Date.now();
      transport.drums = true;
      transport.playing = true;
    } else if (sec.drums === false) transport.playing = false;
    transport.pattern = current.path;
    transport.pattern_edited = current.edited;
    return warnings;
  };
  /** select / section / next / prev / control keys: broadcasts as the engine does */
  const goSection = (i: number): string[] | null => {
    if (!active || i < 0 || i >= active.song.sections.length) return null;
    active.section = i;
    const w = applySection();
    broadcast(songState());
    broadcastState();
    return w;
  };
  // ─────────────── chord memory (ARCHITECTURE §17) ───────────────
  /** Input notes of a layer's MIDI chain = the held keys inside its zone (the mock has no real MIDI path). */
  const heldIn = (l: Patch['layers'][number]) => [...held.keys()].filter((n) => n >= l.zone.key_lo && n <= l.zone.key_hi).sort((a, b) => a - b);

  // ─────────────── server ───────────────
  const wss = new WebSocketServer({
    host,
    port: opts.port ?? MOCK_PORT,
    verifyClient: (info: { origin?: string }) => !info.origin || ALLOWED_ORIGIN.test(info.origin) || info.origin === WEB_ORIGIN,
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

  function applyPreset(p: MockPreset) {
    const keepRhythm = !p.patch.rhythm ? patch.rhythm : undefined; // presets without rhythm keep the current one
    patch = assignNodes(keepRhythm ? { ...p.patch, rhythm: keepRhythm } : p.patch);
    presetPath = p.path;
    dirty = false;
    if (patch.tempo) transport.tempo = patch.tempo;
    energy.clear();
  }
  /** Make `path` the current pattern ("" = empty); false when it does not exist. */
  function applyPattern(path: string): boolean {
    const found = path ? patterns.find((x) => x.entry.path === path) : undefined;
    if (path && !found) return false;
    current.path = path;
    current.edited = false;
    current.pattern = found ? structuredClone(found.pattern) : emptyPattern();
    if (current.pattern.tempo) transport.tempo = current.pattern.tempo;
    if (current.pattern.kit !== undefined && patch.rhythm?.kit) patch.rhythm.kit.kit = current.pattern.kit;
    if (patch.rhythm) patch.rhythm.pattern = path;
    transport.swing = current.pattern.swing;
    transport.time_sig = [...current.pattern.time_sig];
    return true;
  }

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
    const trust = trustOf.get(ws);
    if (trust === 'web' || trust === 'remote') {
      const why = trust === 'remote' && REMOTE_DENIED.has(String(m.type)) ? `'${String(m.type)}' is not available to remote sessions` : webForbidden(m);
      if (why) return err(ws, id, 'forbidden', why);
    }
    switch (m.type) {
      case 'hello':
        send(ws, { ...stateMsg(id), version: MOCK_VERSION, engine: { product: 'keysynth', kind: 'mock', version: MOCK_VERSION, pid: process.pid }, trust: trustOf.get(ws) ?? 'local', cloud: cloudStatus() });
        send(ws, syncStatus());
        send(ws, songState());
        return;
      case 'cloud_link': {
        const token = typeof m.token === 'string' ? m.token : '';
        if (!TOKEN_RE.test(token)) return err(ws, id, 'bad_request', 'malformed device token');
        setTimeout(() => {
          if (token.includes('BAD')) return err(ws, id, 'token_invalid', 'the server rejected the token');
          const next = token.includes('OTHER')
            ? { accountId: '42', accountName: 'Other' }
            : { accountId: '17', accountName: 'Max' };
          if (link && link.accountId !== next.accountId && m.replace !== true)
            return err(ws, id, 'account_mismatch', `this engine is linked to account '${link.accountName}'`);
          const switched = link?.accountId !== next.accountId;
          const first = !seenAccounts.has(next.accountId);
          seenAccounts.add(next.accountId);
          link = { ...next, expiresAt: '2027-04-10T12:00:00Z' };
          send(ws, { type: 'cloud_status', id, ...cloudStatus(), ...(first ? { merged: { presets: presets.filter((p) => !p.factory).length, patterns: 0 } } : {}) });
          broadcast({ type: 'cloud_status', ...cloudStatus() }, ws);
          if (switched) {
            if (presetPath?.startsWith('userdata/')) presetPath = null;
            if (active?.path.startsWith('userdata/')) closeSong();
            broadcastState();
          }
          runSync();
          autoFetch();
        }, 150);
        return;
      }
      case 'cloud_unlink': {
        const had = !!link;
        link = null;
        syncState = 'disabled';
        for (const j of [...jobs]) endJob(j, true);
        send(ws, { type: 'cloud_status', id, ...cloudStatus() });
        broadcast({ type: 'cloud_status', ...cloudStatus() }, ws);
        broadcast(syncStatus());
        if (had && active?.path.startsWith('userdata/')) closeSong();
        if (had) broadcastState();
        return;
      }
      case 'sync_now':
        if (!link) return err(ws, id, 'not_linked', 'link this engine to your account first');
        send(ws, { type: 'sync_now_ok', id });
        runSync();
        return;
      case 'samples_list':
        setTimeout(() => send(ws, samplesList(id)), link ? 120 : 0);
        return;
      case 'samples_fetch': {
        const scope = String(m.scope ?? '');
        const target = typeof m.target === 'string' ? m.target : '';
        if (!['entry', 'library', 'all'].includes(scope) || (scope !== 'all' && !target)) return err(ws, id, 'bad_request', 'scope/target');
        if (!link) return err(ws, id, 'not_linked', 'link this engine to your account first');
        const r = startJob(scope, target);
        if (!r) return err(ws, id, 'not_found', `unknown ${scope} '${target}'`);
        send(ws, { type: 'samples_fetch_ok', id, ...r });
        return;
      }
      case 'samples_cancel': {
        const which = typeof m.job === 'string' ? jobs.filter((j) => j.id === m.job) : [...jobs];
        for (const j of which) endJob(j, true);
        send(ws, { type: 'samples_cancel_ok', id, jobs: which.map((j) => j.id) });
        return;
      }
      case 'samples_delete': {
        const scope = String(m.scope ?? '');
        const target = typeof m.target === 'string' ? m.target : '';
        const es = targetEntries(scope, target);
        if (!es.length) return err(ws, id, 'not_found', `unknown ${scope} '${target}'`);
        const inUse = patch.layers.map((l) => (l.instrument.state as { sfz?: string } | undefined)?.sfz);
        if (es.some((e) => inUse.includes(e.sfz))) return err(ws, id, 'in_use', 'used by the current patch: unload it first');
        if (es.every((e) => e.local)) return err(ws, id, 'read_only', 'local sample library');
        for (const j of [...jobs]) if (j.entries.some((x) => es.some((e) => e.id === x))) endJob(j, true);
        let freed = 0;
        for (const e of es) if (!e.local) { freed += e.cachedBytes; e.cachedBytes = 0; }
        send(ws, { type: 'samples_delete_ok', id, freedBytes: freed });
        return;
      }
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
        applyPreset(p);
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
        if (mod?.kind === 'midi') {
          // MIDI processors go into the layer's MIDI chain (refused on the master chain), ≤ 8
          const l = layerId === 0 ? undefined : findLayer(layerId ?? -1);
          if (!l) return err(ws, id, 'bad_request', 'MIDI processors need a layer (not the master chain)');
          const chain = (l.midi ??= []);
          if (chain.length >= 8) return err(ws, id, 'bad_request', 'at most 8 MIDI processors per layer');
          const idx = num('index') ?? chain.length;
          chain.splice(Math.max(0, Math.min(idx, chain.length)), 0, { ...makeFx(mod.typeId), node: nextNode++ });
          return structuralChange(ws, id);
        }
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
        for (const l of patch.layers) if (l.midi && !l.midi.length) delete l.midi; // written only when not empty
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
        // control keys: while a song is selected their note-ons switch sections and are not played
        const keys = effectiveKeys();
        if (active && keys.enabled && (note === keys.prev || note === keys.next)) {
          if (m.on) goSection(active.section + (note === keys.next ? 1 : -1));
          return;
        }
        noteEvent(!!m.on, note, Math.max(1, Math.min(127, num('velocity') ?? 100)));
        return;
      }
      case 'cc':
        return;
      case 'transport': {
        let all = false;
        if (m.time_sig !== undefined) {
          const ts = m.time_sig as unknown;
          if (!Array.isArray(ts) || ts.length !== 2 || !(ts[0] >= 1 && ts[0] <= 16) || ![2, 4, 8, 16].includes(ts[1] as number))
            return err(ws, id, 'bad_request', 'time_sig: num 1..16, den 2/4/8/16');
        }
        if (typeof m.pattern === 'string') {
          if (!applyPattern(m.pattern)) return err(ws, id, 'not_found', `pattern not found: ${m.pattern}`);
          all = true;
        }
        if (Array.isArray(m.time_sig) && m.time_sig.length === 2) {
          const [num, den] = m.time_sig as [number, number];
          if (!(num >= 1 && num <= 16) || ![2, 4, 8, 16].includes(den)) return err(ws, id, 'bad_request', 'time_sig: num 1..16, den 2/4/8/16');
          current.pattern = setTimeSig(current.pattern, num, den);
          current.edited = true;
          transport.time_sig = [num, den];
          all = true;
        }
        if (m.playing === true && !transport.playing) playStartedAt = Date.now();
        for (const k of ['playing', 'tempo', 'metronome', 'metronome_volume', 'drums', 'drums_volume', 'swing', 'count_in'] as const) {
          if (k in m && m[k] !== null && m[k] !== undefined) (transport as unknown as Record<string, unknown>)[k] = m[k];
        }
        if (typeof m.swing === 'number') current.pattern.swing = Math.min(1, Math.max(0, m.swing));
        transport.tempo = Math.min(400, Math.max(20, transport.tempo));
        transport.pattern = current.path;
        transport.pattern_edited = current.edited;
        if (all) broadcast(stateMsg());
        else broadcast(stateMsg(), ws);
        return;
      }
      case 'list_patterns':
        send(ws, { type: 'list_patterns_ok', id, patterns: patterns.map((p) => p.entry) });
        return;
      case 'get_pattern': {
        const want = typeof m.path === 'string' && m.path && m.path !== current.path ? patterns.find((p) => p.entry.path === m.path) : undefined;
        if (typeof m.path === 'string' && m.path && m.path !== current.path && !want) return err(ws, id, 'not_found', 'no such pattern');
        const snap = want ? { path: want.entry.path, edited: false, pattern: want.pattern } : current;
        send(ws, { type: 'get_pattern_ok', id, ...snap });
        return;
      }
      case 'set_pattern': {
        if (!m.pattern || typeof m.pattern !== 'object' || Array.isArray(m.pattern)) return err(ws, id, 'bad_request', 'pattern must be an object');
        const before = current.pattern.time_sig;
        current.pattern = parsePatternJson(m.pattern as Record<string, unknown>);
        current.edited = true;
        transport.pattern_edited = true;
        transport.swing = current.pattern.swing;
        send(ws, { type: 'set_pattern_ok', id, warnings: [] });
        broadcast({ type: 'pattern', ...current }, ws);
        if (before[0] !== current.pattern.time_sig[0] || before[1] !== current.pattern.time_sig[1]) {
          transport.time_sig = [...current.pattern.time_sig];
          broadcast(stateMsg());
        }
        return;
      }
      case 'save_pattern': {
        const name = typeof m.name === 'string' ? m.name.trim() : '';
        if (!name) return err(ws, id, 'bad_request', 'name required');
        const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
        const p = `userdata/patterns/${slug}.json`;
        const existing = patterns.findIndex((x) => x.entry.path === p);
        if (existing >= 0 && !m.overwrite) return err(ws, id, 'exists', `pattern "${name}" already exists`);
        const pattern: Pattern = { ...structuredClone(current.pattern), name };
        const entry = { entry: { path: p, name, time_sig: pattern.time_sig, bars: pattern.bars, tempo: pattern.tempo ?? 0, factory: false }, pattern };
        if (existing >= 0) patterns[existing] = entry;
        else patterns.push(entry);
        current.path = p;
        current.edited = false;
        current.pattern = structuredClone(pattern);
        transport.pattern = p;
        transport.pattern_edited = false;
        if (patch.rhythm) patch.rhythm.pattern = p;
        send(ws, { type: 'save_pattern_ok', id, path: p });
        broadcast(stateMsg());
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
        const isAsio = t.type === 'ASIO';
        const requested = num('buffer_size');
        if (audio.panelOpen) return err(ws, id, 'busy', "close the driver's control panel first");
        let bs = requested ?? audio.bufferSize;
        const rejected = isAsio && !ASIO_BUFFER_SIZES.includes(bs);
        if (rejected) bs = ASIO_PREFERRED_BUFFER;
        audio = {
          type: t.type,
          name,
          sampleRate: sr,
          bufferSize: bs,
          inputLatencyMs: +((bs / sr) * 1000 + (isAsio ? 0.3 : 5)).toFixed(2),
          outputLatencyMs: +((bs / sr) * 1000 + (isAsio ? 0.9 : 10)).toFixed(2),
          running: true,
          hasControlPanel: isAsio,
        };
        for (const c of wss.clients) send(c, c === ws ? devicesMsg(id) : devicesMsg());
        if (rejected && requested !== undefined) {
          send(ws, {
            type: 'log',
            level: 'warn',
            notify: true,
            message: `the driver did not accept a buffer of ${requested} samples (now ${bs}; offered: ${ASIO_BUFFER_SIZES.join(', ')}). Set the buffer size in the driver's control panel (Open ASIO panel).`,
          });
        }
        broadcastState();
        return;
      }
      case 'open_audio_panel': {
        if (!audio.hasControlPanel) return err(ws, id, 'not_available', 'this audio device has no control panel');
        if (audio.panelOpen) return err(ws, id, 'busy', "the driver's control panel is already open");
        send(ws, { type: 'open_audio_panel_ok', ...(id !== undefined ? { id } : {}) });
        // Simulates a modal driver panel: everyone sees panelOpen, then the user picks 128 samples, the panel closes
        // and the device restarts.
        audio = { ...audio, panelOpen: true };
        for (const c of wss.clients) send(c, devicesMsg());
        broadcastState();
        setTimeout(() => {
          const bs = 128;
          audio = { ...audio, panelOpen: false, bufferSize: bs, outputLatencyMs: +((bs / audio.sampleRate) * 1000 + 0.9).toFixed(2) };
          for (const c of wss.clients) send(c, devicesMsg());
          broadcastState();
          broadcast({
            type: 'log',
            level: 'info',
            message: `audio: ${audio.name}, ${audio.sampleRate} Hz, buffer ${bs} samples, output latency ${audio.outputLatencyMs.toFixed(1)} ms`,
          });
        }, 300);
        return;
      }
      case 'restart_audio': {
        if (audio.panelOpen) return err(ws, id, 'busy', "close the driver's control panel first");
        audio = { ...audio, running: true };
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
      case 'chord_learn': {
        const node = num('node') ?? -1;
        const l = patch.layers.find((x) => x.midi?.some((f) => f.node === node));
        const s = l?.midi?.find((f) => f.node === node);
        if (!l || !s) return err(ws, id, 'not_found', `no MIDI processor ${node}`);
        if (s.type !== 'chord') return err(ws, id, 'bad_request', `node ${node} is not a chord processor`);
        const notes = heldIn(l).slice(0, 8);
        if (notes.length < 2) return err(ws, id, 'no_notes', 'hold 2 or more notes on the layer');
        const intervals = notes.map((n) => Math.min(48, n - notes[0]!));
        const changes: [string, number][] = [['mem_count', intervals.length], ...intervals.map((v, i): [string, number] => [`mem_${i + 1}`, v]), ['type', 12]];
        for (const [param, value] of changes) s.params[param] = value;
        dirty = true;
        send(ws, { type: 'chord_learn_ok', id, node, intervals });
        for (const [param, value] of changes) broadcast({ type: 'param', node, param, value });
        return;
      }
      case 'song_list':
        send(ws, { type: 'song_list_ok', id, songs: songList() });
        return;
      case 'song_get': {
        const s = songs.find((x) => x.path === m.path);
        if (!s) return err(ws, id, 'not_found', `song not found: ${String(m.path)}`);
        send(ws, { type: 'song_get_ok', id, path: s.path, song: s.song });
        return;
      }
      case 'song_save': {
        const raw = m.song as Record<string, unknown> | undefined;
        if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return err(ws, id, 'bad_request', 'song: object required');
        const why = songProblem(raw);
        if (why) return err(ws, id, 'bad_request', why);
        const sid = typeof raw.id === 'string' && SONG_SLUG.test(raw.id) && raw.id.length <= 64 ? raw.id : songSlug(String(raw.title));
        if (!sid) return err(ws, id, 'bad_request', 'id: title has no slug, set an id');
        const path = `userdata/songs/${sid}.json`;
        const i = songs.findIndex((x) => x.path === path);
        if (i >= 0 && m.overwrite !== true) return err(ws, id, 'exists', `song '${sid}' already exists`);
        const song = { ...structuredClone(raw), format: 1, id: sid } as Song;
        if (i >= 0) songs[i] = { path, factory: false, song };
        else songs.push({ path, factory: false, song });
        send(ws, { type: 'song_save_ok', id, path, warnings: [] });
        broadcast({ type: 'songs_changed' }, ws);
        if (active?.path === path) {
          active = { path, song, section: Math.min(active.section, song.sections.length - 1) };
          broadcast(songState());
        }
        runSync();
        return;
      }
      case 'song_delete': {
        const i = songs.findIndex((x) => x.path === m.path);
        if (i < 0) return err(ws, id, 'not_found', `song not found: ${String(m.path)}`);
        if (songs[i]!.factory) return err(ws, id, 'read_only', 'factory songs are read-only');
        songs.splice(i, 1);
        send(ws, { type: 'song_delete_ok', id, path: m.path });
        broadcast({ type: 'songs_changed' }, ws);
        if (active?.path === m.path) closeSong();
        runSync();
        return;
      }
      case 'song_select': {
        const s = songs.find((x) => x.path === m.path);
        if (!s) return err(ws, id, 'not_found', `song not found: ${String(m.path)}`);
        const section = num('section') ?? 0;
        if (!Number.isInteger(section) || section < 0 || section >= s.song.sections.length) return err(ws, id, 'bad_request', 'section out of range');
        active = { path: s.path, song: structuredClone(s.song), section };
        const warnings = applySection();
        send(ws, { type: 'song_select_ok', id, warnings });
        broadcast(songState());
        broadcastState();
        return;
      }
      case 'song_section':
      case 'song_next':
      case 'song_prev': {
        if (!active) return err(ws, id, 'no_song', 'no song selected');
        const n = active.song.sections.length;
        const target = m.type === 'song_section' ? (num('index') ?? -1) : active.section + (m.type === 'song_next' ? 1 : -1);
        if (m.type === 'song_section' && (target < 0 || target >= n)) return err(ws, id, 'bad_request', 'index out of range');
        if (target < 0 || target >= n) {
          send(ws, { type: 'song_section_ok', id, warnings: [] }); // first / last section: nothing changes
          return;
        }
        active.section = target;
        const warnings = applySection();
        send(ws, { type: 'song_section_ok', id, warnings });
        broadcast(songState());
        broadcastState();
        return;
      }
      case 'song_close':
        send(ws, { type: 'song_close_ok', id });
        active = null;
        broadcast(songState());
        return;
      case 'song_control_keys': {
        if (m.enabled !== undefined && typeof m.enabled !== 'boolean') return err(ws, id, 'bad_request', 'enabled: bool');
        if ((m.prev !== undefined && !keyOk(m.prev)) || (m.next !== undefined && !keyOk(m.next))) return err(ws, id, 'bad_request', 'prev/next: 0..127 or null');
        globalKeys = {
          enabled: typeof m.enabled === 'boolean' ? m.enabled : globalKeys.enabled,
          prev: m.prev !== undefined ? (m.prev as number | null) : globalKeys.prev,
          next: m.next !== undefined ? (m.next as number | null) : globalKeys.next,
        };
        send(ws, { type: 'song_control_keys_ok', id });
        broadcast(songState());
        return;
      }
      default:
        return err(ws, id, 'unknown_type', `unknown message type: ${String(m.type)}`);
    }
  }

  wss.on('connection', (ws, req) => {
    trustOf.set(ws, req.headers[RELAY_HEADER] === '1' ? 'remote' : req.headers.origin === WEB_ORIGIN ? 'web' : 'local');
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
        const t = (m as { type: string }).type;
        if (t === 'set_patch' || t === 'load_preset' || t === 'set_instrument') autoFetch();
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
      for (const f of l.midi ?? []) {
        if (f.type !== 'chord') continue;
        const h = heldIn(l);
        const r: Record<string, number> = { held_count: Math.min(16, h.length) };
        for (let i = 0; i < 8; i++) r[`held_${i + 1}`] = h[i] ?? -1;
        readouts[String(f.node)] = r;
      }
      if (l.instrument.type === 'va') readouts[String(l.instrument.node)] = { active_voices: Math.min(16, held.size) };
      if (l.instrument.type === 'sampler') {
        // like the engine: a downloadable entry that is not cached and no job covers it → not linked (1)
        const sfz = (l.instrument.state as { sfz?: string } | undefined)?.sfz ?? '';
        const e = allEntries().find((x) => `assets/samples/${x.id}` === sfz);
        const missing = !!e && !e.local && e.cachedBytes < e.bytes && !link && !jobs.some((j) => j.id === `entry:${e.id}`);
        readouts[String(l.instrument.node)] = { loading: 0, samples_missing: missing ? 1 : 0 };
      }
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
    // sequencer position (count-in = one bar of clicks before step 0)
    let step = -1;
    let bar = -1;
    let ppq = 0;
    if (transport.playing) {
      const pt = current.pattern;
      ppq = ((Date.now() - playStartedAt) / 1000) * (transport.tempo / 60);
      const stepPpq = 4 / (pt.time_sig[1] * pt.steps_per_beat);
      const origin = transport.count_in ? (4 * pt.time_sig[0]) / pt.time_sig[1] : 0;
      if (ppq >= origin) {
        step = Math.floor((ppq - origin) / stepPpq) % numSteps(pt);
        bar = Math.floor(step / (pt.time_sig[0] * pt.steps_per_beat));
        const hits = transport.drums ? pt.tracks.filter((t) => !t.mute && (t.steps[step] ?? 0) > 0).length : 0;
        masterLin += hits * 0.12 * (transport.drums_volume ?? 0.8);
      }
    }
    broadcast({
      type: 'telemetry',
      cpu: Math.min(1, 0.04 + voices * 0.012 + Math.random() * 0.02),
      xruns,
      voices,
      meters: { master: [toDb(masterLin * 0.98), toDb(masterLin)], layers },
      readouts,
      transport: { ppq, playing: transport.playing, step, bar },
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
      const port = typeof addr === 'object' && addr ? addr.port : (opts.port ?? MOCK_PORT);
      log(`listening on ws://${host}:${port}${opts.demo ? ' (demo)' : ''}`);
      resolve({
        port,
        get patch() {
          return patch;
        },
        close: () =>
          new Promise<void>((res) => {
            clearInterval(telemetryTimer);
            clearInterval(samplesTimer);
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
    port: Number(val('--port') ?? MOCK_PORT),
    demo: args.includes('--demo'),
    latencyMs: Number(val('--latency') ?? 0),
  }).catch((e) => {
    console.error('[mock] failed to start:', e);
    process.exit(1);
  });
}
