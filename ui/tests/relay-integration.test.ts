// End-to-end over real sockets: EngineClient + stores → RelayLink → mock relay (mock/relay-mock.ts) → mock engine,
// next to a direct client on the same engine (the PC tab).
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import WebSocket from 'ws';
import { EngineClient, type WebSocketLike } from '../src/protocol/client';
import { bindClient, createEngineStore } from '../src/store/engineStore';
import { createCloudStore } from '../src/store/cloudStore';
import { RelayLink } from '../src/platform/relay';
import { MASTER_NODE } from '../src/protocol/types';
import { startMockEngine, type MockEngine } from '../mock/mock-engine';
import { RELAY_PATH, startRelayMock, type RelayMock } from '../mock/relay-mock';

let engine: MockEngine;
let hub: RelayMock;
let link: RelayLink;
let remote: EngineClient;
let direct: EngineClient;
let store: ReturnType<typeof createEngineStore>;
let pcStore: ReturnType<typeof createEngineStore>;
let cloud: ReturnType<typeof createCloudStore>;

const until = async (cond: () => boolean, ms = 3000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timeout waiting for condition');
    await new Promise((r) => setTimeout(r, 10));
  }
};

beforeAll(async () => {
  engine = await startMockEngine({ port: 0, quiet: true, telemetryHz: 30 });
  hub = await startRelayMock({ port: 0, engineUrl: `ws://127.0.0.1:${engine.port}`, hosts: 2, quiet: true });
  link = new RelayLink({
    url: `ws://127.0.0.1:${hub.port}${RELAY_PATH}`,
    createSocket: (u) => new WebSocket(u) as unknown as WebSocketLike,
    jitter: 0,
  });
  remote = new EngineClient({ createSocket: () => link.transport(), backoffInitialMs: 50, jitter: 0 });
  store = createEngineStore({ client: remote, schedule: (fn) => setTimeout(fn, 0) });
  bindClient(store, remote);
  cloud = createCloudStore({ client: remote, mode: 'platform', api: { mint: () => Promise.reject(new Error('no')), listDevices: () => Promise.resolve([]) } });
  cloud.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });

  direct = new EngineClient({
    url: `ws://127.0.0.1:${engine.port}`,
    createSocket: (u) => new WebSocket(u, { origin: 'http://localhost:5173' }) as unknown as WebSocketLike,
  });
  pcStore = createEngineStore({ client: direct, schedule: (fn) => setTimeout(fn, 0) });
  bindClient(pcStore, direct);
  direct.connect();
  await until(() => pcStore.getState().patch !== null);

  link.start();
  remote.connect();
  await until(() => link.getState().status === 'open');
});

afterAll(async () => {
  remote.close();
  direct.close();
  link.stop();
  await hub.close();
  await engine.close();
});

describe('controller through the relay mock', () => {
  it('two hosts online: waits for a choice, then bootstraps with trust remote', async () => {
    expect(link.getState().hosts.map((h) => h.name)).toEqual(['Mock PC', 'Mock PC 2']);
    expect(link.getState().hostId).toBeNull();
    expect(remote.status).not.toBe('open');
    link.selectHost('mock-1');
    await until(() => store.getState().catalog.length > 0 && store.getState().presets.length > 0 && !!store.getState().devices);
    expect(store.getState().identity).toBe('mock');
    await until(() => cloud.getState().trust === 'remote');
    expect(cloud.getState().plan()).toEqual({ kind: 'idle', reason: 'remote' });
  });

  it('remote-denied messages come back forbidden', async () => {
    await expect(remote.request({ type: 'cloud_unlink' })).rejects.toMatchObject({ code: 'forbidden' });
    await expect(remote.request({ type: 'open_audio_panel' })).rejects.toMatchObject({ code: 'forbidden' });
  });

  it('PC tab and controller stay in sync both ways', async () => {
    store.getState().setParam(MASTER_NODE, 'volume_db', -12);
    await until(() => pcStore.getState().patch?.master.volume_db === -12);
    pcStore.getState().setTransport({ tempo: 133 });
    await until(() => store.getState().transport.tempo === 133);
    const path = store.getState().presets[1]!.path;
    await store.getState().loadPreset(path);
    await until(() => pcStore.getState().presetPath === path);
  });

  it('pause stops telemetry for this controller; resume restarts it', async () => {
    let n = 0;
    const off = remote.on('telemetry', () => n++);
    await until(() => n > 3);
    link.setVisible(false);
    await until(() => hub.stats.pause === 1);
    await new Promise((r) => setTimeout(r, 60));
    const frozen = n;
    await new Promise((r) => setTimeout(r, 150));
    expect(n).toBe(frozen);
    link.setVisible(true);
    await until(() => n > frozen + 2);
    off();
  });

  it('host offline → pending rejects; back online → resync (hello) on the same host', async () => {
    hub.setOnline('mock-2', false);
    await until(() => link.getState().hosts.filter((h) => h.online).length === 1);
    const helloBefore = hub.lastMsgs.filter((m) => m.type === 'hello').length;
    const p = remote.request({ type: 'get_catalog' }, { timeoutMs: 3000 });
    hub.setOnline('mock-1', false);
    await expect(p).rejects.toMatchObject({ code: 'disconnected' });
    await until(() => remote.status === 'reconnecting');
    expect(link.getState().hostId).toBe('mock-1'); // kept while offline («не в сети»)
    hub.setOnline('mock-1', true);
    await until(() => remote.status === 'open' && store.getState().identity === 'mock');
    expect(hub.lastMsgs.filter((m) => m.type === 'hello').length).toBe(helloBefore + 1);
    expect(link.getState().hostId).toBe('mock-1');
  });
});
