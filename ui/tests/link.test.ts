// Account link: pure plan + mint/link helper, then the cloud store against the mock engine (a `web` session,
// Origin https://mvv42.ru) with a mocked REST API (POST/GET /keysynth/api/devices).
import { afterAll, afterEach, beforeAll, describe, expect, it, vi } from 'vitest';
import WebSocket from 'ws';
import { EngineClient, EngineError, type WebSocketLike } from '../src/protocol/client';
import { bindClient, createEngineStore } from '../src/store/engineStore';
import { createCloudStore, type CloudStore } from '../src/store/cloudStore';
import { linkEngine, ownerFromDevices, planLink, type PlanInput } from '../src/platform/link';
import { startMockEngine, type MockEngine } from '../mock/mock-engine';
import type { CloudStatus } from '../src/protocol/types';

const NOW = Date.parse('2026-10-12T12:00:00Z');
const cloud = (o: Partial<CloudStatus> = {}): CloudStatus => ({
  linked: false,
  accountId: null,
  accountName: null,
  deviceId: 'dev-1',
  expiresAt: null,
  online: true,
  error: null,
  ...o,
});
const base: PlanInput = { mode: 'platform', connected: true, cloud: cloud(), meStatus: 'authenticated', granted: true, owner: 'unknown', now: NOW };

describe('planLink', () => {
  it('local mode and no engine → idle', () => {
    expect(planLink({ ...base, mode: 'local' })).toEqual({ kind: 'idle', reason: 'local-mode' });
    expect(planLink({ ...base, connected: false })).toEqual({ kind: 'idle', reason: 'no-engine' });
    expect(planLink({ ...base, cloud: null })).toEqual({ kind: 'idle', reason: 'no-engine' });
  });
  it('auth states', () => {
    expect(planLink({ ...base, meStatus: 'loading' }).kind).toBe('idle');
    expect(planLink({ ...base, meStatus: 'anonymous' }).kind).toBe('login');
    expect(planLink({ ...base, granted: false }).kind).toBe('no-access');
  });
  it('unlinked → link; linked → check owner → ok / foreign / remint near expiry', () => {
    expect(planLink(base).kind).toBe('link');
    const linked = cloud({ linked: true, accountId: '17', accountName: 'Max', expiresAt: '2027-04-10T12:00:00Z' });
    expect(planLink({ ...base, cloud: linked }).kind).toBe('check-owner');
    expect(planLink({ ...base, cloud: linked, owner: 'mine' }).kind).toBe('ok');
    expect(planLink({ ...base, cloud: linked, owner: 'foreign' }).kind).toBe('foreign');
    const soon = cloud({ ...linked, expiresAt: new Date(NOW + 10 * 864e5).toISOString() });
    expect(planLink({ ...base, cloud: soon, owner: 'mine' }).kind).toBe('remint');
  });
  it('owner = our device row with the same expiry', () => {
    const c = cloud({ linked: true, expiresAt: '2027-04-10T12:00:00Z' });
    expect(ownerFromDevices(c, [{ deviceId: 'dev-1', name: 'x', expiresAt: '2027-04-10T12:00:00.000Z' }])).toBe('mine');
    expect(ownerFromDevices(c, [{ deviceId: 'dev-1', name: 'x', expiresAt: '2027-05-01T00:00:00Z' }])).toBe('foreign');
    expect(ownerFromDevices(c, [])).toBe('foreign');
  });
});

describe('linkEngine', () => {
  const minted = { token: 'ksd_' + 'a'.repeat(43), expiresAt: '2027-04-10T12:00:00Z', accountId: '17', accountName: 'Max' };
  it('mints then sends the token to the engine; the token is never part of the result', async () => {
    const sendLink = vi.fn(async () => ({ ...cloud({ linked: true, accountId: '17' }), merged: { presets: 2, patterns: 0 } }));
    const r = await linkEngine({ mint: async () => minted, sendLink }, 'dev-1', 'pc', false);
    expect(sendLink).toHaveBeenCalledWith(minted.token, false);
    expect(r).toMatchObject({ ok: true, merged: { presets: 2, patterns: 0 } });
    expect(JSON.stringify(r)).not.toContain(minted.token);
  });
  it('maps REST and engine errors to codes', async () => {
    const restErr = Object.assign(new Error('too_many_devices (HTTP 409)'), { status: 409, code: 'too_many_devices' });
    expect(await linkEngine({ mint: async () => Promise.reject(restErr), sendLink: vi.fn() }, 'd', 'n', false)).toMatchObject({
      ok: false,
      code: 'too_many_devices',
    });
    const mismatch = new EngineError('account_mismatch', "linked to 'Other'");
    const r = await linkEngine({ mint: async () => minted, sendLink: async () => Promise.reject(mismatch) }, 'd', 'n', false);
    expect(r).toMatchObject({ ok: false, code: 'account_mismatch' });
  });
});

// ─────────────── store + mock engine ───────────────

let engine: MockEngine;
const until = async (cond: () => boolean, ms = 3000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timeout waiting for condition');
    await new Promise((r) => setTimeout(r, 10));
  }
};

beforeAll(async () => {
  engine = await startMockEngine({ port: 0, quiet: true, telemetryHz: 5 });
});
afterAll(async () => {
  await engine.close();
});

interface Rig {
  client: EngineClient;
  store: CloudStore;
  engineStore: ReturnType<typeof createEngineStore>;
  mint: ReturnType<typeof vi.fn>;
  listDevices: ReturnType<typeof vi.fn>;
  sent: string[];
}
const rigs: Rig[] = [];
/** server-side device rows per account (shared by all pages of that account) */
const serverDevices: Record<string, { deviceId: string; name: string; expiresAt: string }[]> = {};
afterEach(() => {
  for (const r of rigs.splice(0)) r.client.close();
});

/** A page from mvv42.ru (web trust) whose REST API is a mock: account 17 «Max» or 42 «Other». */
function rig(account: 'me' | 'other' = 'me'): Rig {
  const sent: string[] = [];
  const client = new EngineClient({
    url: `ws://127.0.0.1:${engine.port}`,
    createSocket: (u) => {
      const ws = new WebSocket(u, { origin: 'https://mvv42.ru' });
      const orig = ws.send.bind(ws);
      (ws as unknown as { send: (d: string) => void }).send = (d: string) => {
        sent.push(d);
        orig(d);
      };
      return ws as unknown as WebSocketLike;
    },
  });
  const devices = (serverDevices[account] ??= []);
  let n = 0;
  const mint = vi.fn(async (deviceId: string, name: string) => {
    const tag = account === 'other' ? 'OTHER' : 'mine0';
    const token = `ksd_${tag}${String(++n).padStart(38, '0')}`;
    const expiresAt = '2027-04-10T12:00:00Z'; // = the mock engine's fixed expiry
    devices.splice(0, devices.length, { deviceId, name, expiresAt });
    return { deviceId, token, expiresAt, accountId: account === 'me' ? '17' : '42', accountName: account === 'me' ? 'Max' : 'Other' };
  });
  const listDevices = vi.fn(async () => [...devices]);
  const store = createCloudStore({ client, mode: 'platform', api: { mint, listDevices }, relistDelayMs: 0, storage: null });
  const engineStore = createEngineStore({ client, schedule: (fn) => setTimeout(fn, 0) });
  bindClient(engineStore, client);
  const r = { client, store, engineStore, mint, listDevices, sent };
  rigs.push(r);
  return r;
}

const login = (s: CloudStore, name = 'Max') => s.getState().setMe({ status: 'authenticated', granted: true, name });

describe('automatic link against the mock engine', () => {
  it('hello → unlinked → mint + cloud_link → linked; sync starts; token not kept in the store', async () => {
    const r = rig();
    r.client.connect();
    await until(() => r.store.getState().cloud !== null);
    expect(r.store.getState().trust).toBe('web');
    expect(r.store.getState().engineVersion).toBe('0.1.0');
    expect(r.store.getState().cloud?.linked).toBe(false);
    login(r.store);
    await until(() => !!r.store.getState().cloud?.linked && !r.store.getState().link.busy);
    expect(r.mint).toHaveBeenCalledTimes(1);
    expect(r.store.getState().cloud).toMatchObject({ linked: true, accountId: '17', accountName: 'Max' });
    expect(r.store.getState().link.owner).toBe('mine');
    const token = (await r.mint.mock.results[0]!.value).token as string;
    expect(r.sent.some((m) => m.includes('"cloud_link"') && m.includes(token))).toBe(true);
    expect(JSON.stringify(r.store.getState())).not.toContain(token);
    await until(() => r.store.getState().sync?.state === 'idle' && !!r.store.getState().sync?.lastSyncAt);
  });

  it('a second page sees the link as its own (device list) and does not re-mint', async () => {
    const r = rig();
    r.client.connect();
    login(r.store);
    await until(() => r.store.getState().link.owner === 'mine');
    expect(r.listDevices).toHaveBeenCalled();
    expect(r.mint).not.toHaveBeenCalled();
    expect(r.store.getState().plan().kind).toBe('ok');
  });

  it('another account: detected as foreign, never relinked silently; confirm → replace', async () => {
    const r = rig('other');
    r.client.connect();
    login(r.store, 'Other');
    await until(() => r.store.getState().link.owner === 'foreign');
    expect(r.mint).not.toHaveBeenCalled();
    expect(r.store.getState().cloud?.accountName).toBe('Max');
    // the user asks to link → engine says account_mismatch → confirmation pending, nothing changed
    await r.store.getState().retryLink();
    expect(r.store.getState().link.confirm).toEqual({ current: 'Max' });
    expect(r.store.getState().cloud?.accountId).toBe('17');
    await r.store.getState().confirmReplace();
    await until(() => r.store.getState().cloud?.accountId === '42');
    expect(r.sent.some((m) => m.includes('"replace":true'))).toBe(true);
    expect(r.store.getState().link).toMatchObject({ owner: 'mine', confirm: null, busy: false });
  });

  it('unlink holds: no automatic re-link until the user asks', async () => {
    const r = rig('other');
    r.client.connect();
    login(r.store, 'Other');
    await until(() => r.store.getState().link.owner === 'mine');
    await r.store.getState().unlink();
    expect(r.store.getState().cloud?.linked).toBe(false);
    expect(r.store.getState().link.held).toBe(true);
    await new Promise((res) => setTimeout(res, 250));
    expect(r.store.getState().cloud?.linked).toBe(false);
    expect(r.mint).not.toHaveBeenCalled();
    await r.store.getState().retryLink();
    await until(() => !!r.store.getState().cloud?.linked);
    expect(r.mint).toHaveBeenCalledTimes(1);
  });

  it('anonymous visitor: nothing is minted', async () => {
    const r = rig();
    r.client.connect();
    r.store.getState().setMe({ status: 'anonymous', granted: false, name: null });
    await until(() => r.store.getState().cloud !== null);
    await new Promise((res) => setTimeout(res, 100));
    expect(r.store.getState().plan().kind).toBe('login');
    expect(r.mint).not.toHaveBeenCalled();
  });

  it('samples: list, auto-fetch on preset load with progress, done → cached', async () => {
    const r = rig('other'); // engine is linked to account 42 by now
    r.client.connect();
    login(r.store, 'Other');
    await until(() => r.store.getState().link.owner === 'mine' && r.engineStore.getState().presets.length > 0);
    await r.store.getState().refreshSamples();
    const sv = r.store.getState().samples!;
    expect(sv.manifest.state).toBe('ok');
    expect(sv.groups.map((g) => g.id)).toEqual(['piano', 'epiano']);
    const preset = r.engineStore.getState().presets.find((p) => p.name === 'jRhodes Stereo')!;
    await r.engineStore.getState().loadPreset(preset.path);
    await until(() => !!r.store.getState().jobs['entry:jrhodes3c/jRhodes3c-stereo.sfz']);
    const j = r.store.getState().jobs['entry:jrhodes3c/jRhodes3c-stereo.sfz']!;
    expect(j.bytesTotal).toBe(18 * 1024 * 1024);
    await until(() => r.store.getState().sampleNotice?.kind === 'done', 6000);
    expect(r.store.getState().jobs).toEqual({});
    await until(() => {
      const e = r.store.getState().samples?.groups[1]?.libraries[0]?.entries.find((x) => x.id === 'jrhodes3c/jRhodes3c-stereo.sfz');
      return e?.status === 'cached';
    });
    // in use by the current patch → delete refused with a readable message
    expect(await r.store.getState().deleteSamples('entry', 'jrhodes3c/jRhodes3c-stereo.sfz')).toBe(false);
    expect(r.store.getState().samplesError).toMatch(/текущим патчем/);
  }, 10000);

  it('cancel ends a job with a cancelled notice', async () => {
    const r = rig('other');
    r.client.connect();
    login(r.store, 'Other');
    await until(() => r.store.getState().link.owner === 'mine');
    await r.store.getState().fetchSamples('library', 'salamander');
    await until(() => !!r.store.getState().jobs['library:salamander']);
    await r.store.getState().cancelSamples('library:salamander');
    await until(() => r.store.getState().sampleNotice?.kind === 'cancelled');
    expect(r.store.getState().jobs['library:salamander']).toBeUndefined();
  });
});
