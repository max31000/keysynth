// End-to-end: real EngineClient + store against the mock engine over a real socket.
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import WebSocket from 'ws';
import { EngineClient, type WebSocketLike } from '../src/protocol/client';
import { bindClient, createEngineStore } from '../src/store/engineStore';
import { startMockEngine, type MockEngine } from '../mock/mock-engine';
import { findModule } from '../src/store/patchOps';

let engine: MockEngine;
let client: EngineClient;
let store: ReturnType<typeof createEngineStore>;

const until = async (cond: () => boolean, ms = 2000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timeout waiting for condition');
    await new Promise((r) => setTimeout(r, 10));
  }
};

beforeAll(async () => {
  engine = await startMockEngine({ port: 0, quiet: true, telemetryHz: 30 });
  client = new EngineClient({
    url: `ws://127.0.0.1:${engine.port}`,
    createSocket: (u) => new WebSocket(u, { origin: 'http://localhost:5173' }) as unknown as WebSocketLike,
  });
  store = createEngineStore({ client, schedule: (fn) => setTimeout(fn, 0) });
  bindClient(store, client);
  client.connect();
  await until(() => store.getState().catalog.length > 0 && store.getState().presets.length > 0 && !!store.getState().devices);
});

afterAll(async () => {
  client.close();
  await engine.close();
});

describe('mock engine round trip', () => {
  it('bootstraps state, catalog, presets, devices', () => {
    const s = store.getState();
    expect(s.connection.status).toBe('open');
    expect(s.patch?.layers.length).toBeGreaterThan(0);
    expect(s.catalogMap.va?.params.length).toBeGreaterThanOrEqual(25);
    expect(s.catalog.some((m) => m.params.some((p) => p.flags & 1))).toBe(true);
    expect(s.devices?.types).toContain('ASIO');
  });

  it('loads a preset and receives the new state', async () => {
    const path = store.getState().presets.find((p) => p.name === 'Echoes B3')!.path;
    await store.getState().loadPreset(path);
    await until(() => store.getState().presetPath === path);
    expect(store.getState().patch?.layers[0]?.instrument.type).toBe('organ');
  });

  it('set_param reaches the engine', async () => {
    const inst = store.getState().patch!.layers[0]!.instrument;
    store.getState().setParam(inst.node!, 'drawbar_4', 7);
    await until(() => findModule(engine.patch, inst.node!)?.slot.params.drawbar_4 === 7);
  });

  it('structural ops round-trip', async () => {
    const layer = store.getState().patch!.layers[0]!;
    await store.getState().addFx(layer.node!, 'delay');
    expect(store.getState().patch!.layers[0]!.fx.at(-1)?.type).toBe('delay');
    const delay = store.getState().patch!.layers[0]!.fx.at(-1)!;
    await store.getState().moveFx(delay.node!, 0);
    expect(store.getState().patch!.layers[0]!.fx[0]?.type).toBe('delay');
    await store.getState().addLayer();
    expect(store.getState().patch!.layers).toHaveLength(2);
  });

  it('errors come back as EngineError', async () => {
    await expect(client.request({ type: 'load_preset', path: 'presets/nope.json' })).rejects.toMatchObject({ code: 'not_found' });
  });

  it('audio: rejected buffer size toasts, the panel reports panelOpen and refuses a second open', async () => {
    store.getState().clearError();
    await store.getState().setAudioDevice('ASIO', 'Focusrite USB ASIO', 48000, 32);
    await until(() => store.getState().lastError !== null);
    expect(store.getState().lastError).toMatch(/did not accept a buffer of 32/);
    expect(store.getState().audio?.bufferSize).toBe(256);
    store.getState().clearError();

    await store.getState().openAudioPanel();
    await until(() => store.getState().audio?.panelOpen === true);
    await expect(client.request({ type: 'open_audio_panel' })).rejects.toMatchObject({ code: 'busy' });
    await until(() => store.getState().audio?.panelOpen === false && store.getState().audio?.bufferSize === 128);
    await until(() => store.getState().logs.some((l) => l.level === 'info' && l.message.includes('buffer 128')));
    expect(store.getState().lastError).toBeNull(); // info logs never toast
  });

  it('save_preset adds a user preset', async () => {
    const path = await store.getState().savePreset('My Test Patch', 'Organ');
    expect(path).toBe('userdata/presets/my-test-patch.json');
    expect(store.getState().presets.some((p) => p.path === path && !p.factory)).toBe(true);
  });

  it('rhythm: patterns listed, load applies meter/tempo, step edits reach the engine, playhead streams', async () => {
    await until(() => store.getState().patterns.length >= 10 && !!store.getState().pattern);
    const money = store.getState().patterns.find((p) => p.path.endsWith('money-7-4.json'))!;
    store.getState().loadPattern(money.path);
    await until(() => store.getState().pattern?.path === money.path);
    expect(store.getState().pattern!.pattern.time_sig).toEqual([7, 4]);
    expect(store.getState().transport.tempo).toBe(123);
    store.getState().editPattern((p) => {
      const tracks = p.tracks.map((t, i) => (i === 0 ? { ...t, steps: t.steps.map((v, s) => (s === 1 ? 99 : v)) } : t));
      return { ...p, tracks };
    });
    await new Promise((r) => setTimeout(r, 30)); // let the coalesced set_pattern flush
    const got = await client.request({ type: 'get_pattern' }, { expect: 'get_pattern_ok' });
    expect(got.pattern.tracks[0]!.steps[1]).toBe(99);
    expect(got.edited).toBe(true);
    // kit param on the rhythm node
    const kit = store.getState().patch!.rhythm!.node!;
    store.getState().setParam(kit, 'kick_tune', 3);
    await until(() => engine.patch.rhythm?.kit?.kick_tune === 3);
    // play → telemetry carries a step
    store.getState().setTransport({ playing: true });
    let step = -1;
    const off = client.on('telemetry', (t) => (step = t.transport?.step ?? -1));
    await until(() => step >= 0);
    off();
    store.getState().setTransport({ playing: false });
  });

  it('streams telemetry and midi', async () => {
    let midi = 0;
    let tele = 0;
    const off1 = client.on('midi', () => midi++);
    const off2 = client.on('telemetry', () => tele++);
    store.getState().note(true, 60, 100);
    await until(() => midi > 0 && tele > 2);
    store.getState().note(false, 60, 0);
    off1();
    off2();
  });
});
