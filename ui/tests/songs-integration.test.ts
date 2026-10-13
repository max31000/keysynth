// End-to-end against the mock engine: MIDI chain (add_fx kind midi, chord_learn) and songs (list, select,
// sections, save → songs_changed to the other client, control keys).
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import WebSocket from 'ws';
import { EngineClient, type WebSocketLike } from '../src/protocol/client';
import { bindClient, createEngineStore } from '../src/store/engineStore';
import { createSongStore } from '../src/store/songStore';
import { startMockEngine, type MockEngine } from '../mock/mock-engine';
import { findModule } from '../src/store/patchOps';
import { savePayload } from '../src/lib/songEdit';

let engine: MockEngine;
const clients: EngineClient[] = [];

const until = async (cond: () => boolean, ms = 2000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timeout waiting for condition');
    await new Promise((r) => setTimeout(r, 10));
  }
};

function connect() {
  const client = new EngineClient({
    url: `ws://127.0.0.1:${engine.port}`,
    createSocket: (u) => new WebSocket(u, { origin: 'http://localhost:5173' }) as unknown as WebSocketLike,
  });
  const store = createEngineStore({ client, schedule: (fn) => setTimeout(fn, 0) });
  bindClient(store, client);
  const songs = createSongStore({ client });
  client.connect();
  clients.push(client);
  return { client, store, songs };
}

let a: ReturnType<typeof connect>;
let b: ReturnType<typeof connect>;

beforeAll(async () => {
  engine = await startMockEngine({ port: 0, quiet: true, telemetryHz: 30 });
  a = connect();
  b = connect();
  await until(() => [a, b].every((c) => c.store.getState().catalog.length > 0 && c.songs.getState().loaded && !!c.songs.getState().state));
});

afterAll(async () => {
  for (const c of clients) c.close();
  await engine.close();
});

describe('MIDI chain on the mock', () => {
  it('catalog has the midi processors', () => {
    const m = a.store.getState().catalogMap;
    expect(['arp', 'chord', 'hold'].map((t) => m[t]?.kind)).toEqual(['midi', 'midi', 'midi']);
    expect(m.arp!.params.find((p) => p.id === 'sync')).toMatchObject({ def: 13, choices: expect.arrayContaining(['1/16']) });
  });

  it('add_fx of a midi module fills layer.midi; master chain refused', async () => {
    const layer = a.store.getState().patch!.layers[0]!.node!;
    await a.store.getState().addFx(layer, 'chord');
    await until(() => (a.store.getState().patch!.layers[0]!.midi?.length ?? 0) === 1);
    expect(a.store.getState().patch!.layers[0]!.midi![0]!.type).toBe('chord');
    await a.store.getState().addFx(0, 'arp');
    expect(a.store.getState().lastError).toMatch(/master/);
    a.store.getState().clearError();
  });

  it('chord_learn: no_notes without held keys, then learns the held notes; params reach every client', async () => {
    const node = a.store.getState().patch!.layers[0]!.midi![0]!.node!;
    expect(await a.store.getState().chordLearn(node)).toBe('no_notes');
    for (const n of [60, 64, 67]) a.store.getState().note(true, n, 100); // same socket: handled before the learn
    expect(await a.store.getState().chordLearn(node)).toBeNull();
    for (const s of [a, b]) {
      await until(() => findModule(s.store.getState().patch, node)?.slot.params.mem_count === 3);
      const p = findModule(s.store.getState().patch, node)!.slot.params;
      expect([p.mem_1, p.mem_2, p.mem_3, p.type]).toEqual([0, 4, 7, 12]);
    }
    for (const n of [60, 64, 67]) a.store.getState().note(false, n, 0);
  });

  it('bypass and remove a MIDI processor by node', async () => {
    const node = a.store.getState().patch!.layers[0]!.midi![0]!.node!;
    a.store.getState().setBypass(node, true);
    await until(() => (findModule(engine.patch, node)?.slot as { bypass?: boolean } | undefined)?.bypass === true);
    await a.store.getState().removeFx(node);
    await until(() => a.store.getState().patch!.layers[0]!.midi === undefined);
  });
});

describe('songs on the mock', () => {
  it('lists factory songs and starts without a song', () => {
    const s = a.songs.getState();
    expect(s.songs.filter((x) => x.factory).length).toBe(2);
    expect(s.state).toMatchObject({ path: null, song: null, section: -1, globalControlKeys: { enabled: false, prev: 21, next: 22 } });
  });

  it('select / next / prev / section broadcast song_state and state to all', async () => {
    const riders = a.songs.getState().songs.find((x) => x.id === 'riders-on-the-storm')!;
    await a.songs.getState().select(riders.path);
    for (const c of [a, b]) await until(() => c.songs.getState().state?.path === riders.path && c.songs.getState().state?.section === 0);
    await until(() => b.store.getState().presetPath === 'presets/factory/e-piano/riders-rhodes.json');
    await b.songs.getState().next();
    await until(() => a.songs.getState().state?.section === 1);
    expect(a.store.getState().transport).toMatchObject({ playing: true, tempo: 104, pattern: 'presets/patterns/riders-shuffle.json' });
    await a.songs.getState().section(2);
    await until(() => b.store.getState().transport.tempo === 106);
    await a.songs.getState().next();
    await a.songs.getState().next(); // last section: no change, no error
    await until(() => a.songs.getState().state?.section === 3);
    expect(a.songs.getState().error).toBeNull();
    await a.songs.getState().prev();
    await until(() => b.songs.getState().state?.section === 2);
  });

  it('control keys switch sections from the keyboard while enabled', async () => {
    await a.songs.getState().setControlKeys({ enabled: true });
    await until(() => b.songs.getState().state?.controlKeys.enabled === true);
    a.store.getState().note(true, 22, 100); // A#0 = next
    await until(() => b.songs.getState().state?.section === 3);
    a.store.getState().note(true, 21, 100); // A0 = prev
    await until(() => b.songs.getState().state?.section === 2);
    await a.songs.getState().setControlKeys({ enabled: false });
  });

  it('save as a user copy: songs_changed re-lists the other client; delete; close', async () => {
    const factory = await a.songs.getState().get('presets/songs/light-my-fire.json');
    expect(factory?.notation).toEqual({ musicxml: 'presets/songs/light-my-fire.musicxml' }); // reserved key kept
    const payload = savePayload({ ...factory!, title: 'Light My Fire (live)' }, 'factory');
    const r = await a.songs.getState().save(payload.song, payload.overwrite);
    expect(r).toEqual({ ok: true, path: 'userdata/songs/light-my-fire.json' });
    await until(() => b.songs.getState().songs.some((x) => x.path === 'userdata/songs/light-my-fire.json' && !x.factory));
    const again = await a.songs.getState().save(payload.song);
    expect(again).toMatchObject({ ok: false, code: 'exists' });
    const bad = await a.songs.getState().save({ ...payload.song, sections: [] });
    expect(bad).toMatchObject({ ok: false, code: 'bad_request' });
    expect(await a.songs.getState().remove('presets/songs/light-my-fire.json')).toBe(false);
    expect(a.songs.getState().error).toBe('Заводскую песню нельзя изменить.');
    expect(await a.songs.getState().remove('userdata/songs/light-my-fire.json')).toBe(true);
    await until(() => !b.songs.getState().songs.some((x) => x.path === 'userdata/songs/light-my-fire.json'));
    await a.songs.getState().close();
    await until(() => b.songs.getState().state?.song === null);
  });
});
