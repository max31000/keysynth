// Engine identity (PROTOCOL.md "Hello additions" → `engine`, src/platform/identity.ts): who answers on the engine
// port, and what the stores do for a mock / a foreign program (incident: a leftover dev mock on 7341 was driven by
// the platform page as if it were the engine).
import { afterAll, afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import WebSocket, { WebSocketServer } from 'ws';
import { EngineClient, type WebSocketLike } from '../src/protocol/client';
import { bindClient, createEngineStore } from '../src/store/engineStore';
import { createCloudStore } from '../src/store/cloudStore';
import { checkIdentity, identityAccepted, identityRejected, OLD_MOCK_DEVICE_ID } from '../src/platform/identity';
import { MOCK_PORT, startMockEngine, type MockEngine } from '../mock/mock-engine';

const cloud = { linked: false, accountId: null, accountName: null, deviceId: 'd-1', expiresAt: null, online: true, error: null };
const state = (extra: object) => ({ type: 'state', patch: {}, presetPath: null, dirty: false, audio: {}, transport: {}, ...extra });

describe('checkIdentity', () => {
  it('real engine, mock, legacy engine', () => {
    const engine = { product: 'keysynth', kind: 'engine', version: '0.1.1', pid: 1 };
    expect(checkIdentity(state({ version: '0.1.1', engine, trust: 'web', cloud }))).toBe('engine');
    expect(checkIdentity(state({ version: '0.1.0', engine: { ...engine, kind: 'mock' }, trust: 'local', cloud }))).toBe('mock');
    // engines before 0.1.1 have no `engine` but the other hello additions
    expect(checkIdentity(state({ version: '0.1.0', trust: 'web', cloud }))).toBe('legacy');
    // ... except the old mock (its fixed device id)
    expect(checkIdentity(state({ version: '0.1.0', trust: 'web', cloud: { ...cloud, deviceId: OLD_MOCK_DEVICE_ID } }))).toBe('mock');
  });

  it('anything else is foreign', () => {
    expect(checkIdentity(null)).toBe('foreign');
    expect(checkIdentity('state')).toBe('foreign');
    expect(checkIdentity({ type: 'hi' })).toBe('foreign');
    expect(checkIdentity(state({}))).toBe('foreign'); // a bare state without hello additions
    expect(checkIdentity(state({ version: '1', engine: { product: 'other', kind: 'engine' }, trust: 'web', cloud }))).toBe('foreign');
    expect(checkIdentity(state({ version: '1', engine: { product: 'keysynth', kind: 'x' }, trust: 'web', cloud }))).toBe('foreign');
    expect(checkIdentity(state({ version: '1', engine: null, trust: 'web', cloud }))).toBe('foreign');
  });

  it('acceptance: production pages only take a real engine, dev builds also the mock', () => {
    for (const dev of [false, true]) {
      expect(identityAccepted('engine', dev)).toBe(true);
      expect(identityAccepted('legacy', dev)).toBe(true);
      expect(identityAccepted('foreign', dev)).toBe(false);
      expect(identityRejected('pending', dev)).toBe(false); // not decided yet: no «not keysynth» flash
    }
    expect(identityAccepted('mock', true)).toBe(true);
    expect(identityRejected('mock', false)).toBe(true);
    expect(identityRejected('foreign', true)).toBe(true);
  });
});

const until = async (cond: () => boolean, ms = 3000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timeout waiting for condition');
    await new Promise((r) => setTimeout(r, 10));
  }
};

let mock: MockEngine;
const clients: EngineClient[] = [];
beforeAll(async () => {
  mock = await startMockEngine({ port: 0, quiet: true, telemetryHz: 5 });
});
afterAll(async () => {
  await mock.close();
});
afterEach(() => {
  for (const c of clients.splice(0)) c.close();
});

function page(url: string, origin = 'https://mvv42.ru', requestTimeoutMs = 5000) {
  const sent: string[] = [];
  const client = new EngineClient({
    url,
    requestTimeoutMs,
    createSocket: (u) => {
      const ws = new WebSocket(u, { origin });
      const orig = ws.send.bind(ws);
      (ws as unknown as { send: (d: string) => void }).send = (d: string) => {
        sent.push(d);
        orig(d);
      };
      return ws as unknown as WebSocketLike;
    },
  });
  clients.push(client);
  const engineStore = createEngineStore({ client, schedule: (fn) => setTimeout(fn, 0) });
  bindClient(engineStore, client);
  return { client, engineStore, sent };
}

describe('identity against servers', () => {
  it('the mock listens on its own port by default and reports kind "mock"', async () => {
    expect(MOCK_PORT).toBe(7441);
    const p = page(`ws://127.0.0.1:${mock.port}`);
    p.client.connect();
    await until(() => p.engineStore.getState().identity !== 'pending');
    expect(p.engineStore.getState().identity).toBe('mock');
    expect(p.engineStore.getState().engineInfo).toMatchObject({ product: 'keysynth', kind: 'mock' });
    expect(p.engineStore.getState().engineInfo?.pid).toBe(process.pid);
  });

  it('a production page never links (sends a device token to) the mock', async () => {
    const p = page(`ws://127.0.0.1:${mock.port}`);
    const mint = vi.fn(async () => ({ deviceId: 'x', token: 'ksd_' + 'a'.repeat(43), expiresAt: '2027-01-01T00:00:00Z', accountId: '17', accountName: 'Max' }));
    const store = createCloudStore({ client: p.client, mode: 'platform', acceptMock: false, api: { mint, listDevices: async () => [] }, storage: null });
    store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    p.client.connect();
    await until(() => p.engineStore.getState().identity === 'mock');
    await new Promise((r) => setTimeout(r, 100));
    expect(store.getState().cloud).toBeNull();
    expect(store.getState().engineVersion).toBeNull();
    expect(mint).not.toHaveBeenCalled();
    expect(p.sent.some((s) => s.includes('cloud_link'))).toBe(false);

    // another client links / unlinks the mock: its cloud_status broadcasts must not drive this page either
    const other = page(`ws://127.0.0.1:${mock.port}`, 'http://localhost:5173');
    other.client.connect();
    await until(() => other.engineStore.getState().identity === 'mock');
    await other.client.request({ type: 'cloud_link', token: 'ksd_' + 'b'.repeat(43) }, { expect: 'cloud_status' });
    await other.client.request({ type: 'cloud_unlink' }, { expect: 'cloud_status' });
    await new Promise((r) => setTimeout(r, 150));
    expect(store.getState().cloud).toBeNull();
    expect(mint).not.toHaveBeenCalled();
    expect(p.sent.some((s) => s.includes('cloud_link'))).toBe(false);
  });

  it('a foreign WebSocket server → foreign, and nothing beyond hello is sent', async () => {
    const wss = new WebSocketServer({ host: '127.0.0.1', port: 0 });
    await new Promise<void>((r) => wss.once('listening', () => r()));
    wss.on('connection', (ws) => ws.on('message', () => ws.send(JSON.stringify({ type: 'hi' }))));
    const port = (wss.address() as { port: number }).port;
    try {
      const p = page(`ws://127.0.0.1:${port}`, 'https://mvv42.ru', 300);
      p.client.connect();
      await until(() => p.engineStore.getState().identity === 'foreign');
      expect(p.engineStore.getState().engineInfo).toBeNull();
      expect(p.sent.map((s) => (JSON.parse(s) as { type: string }).type)).toEqual(['hello']);
      // reconnect (the program was closed) → identity is re-evaluated from scratch
      p.client.close();
      expect(p.engineStore.getState().identity).toBe('pending');
    } finally {
      await new Promise<void>((r) => wss.close(() => r()));
    }
  });
});
