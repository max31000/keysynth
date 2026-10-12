// Cloud store edge cases with a scripted fake engine client (no socket): token drop, remint guard, profile refresh,
// broadcast vs pending reply, token never stored or logged.
import { afterEach, describe, expect, it, vi } from 'vitest';
import { EngineClient, type WebSocketLike } from '../src/protocol/client';
import { createCloudStore } from '../src/store/cloudStore';
import type { CloudStatus, EngineMsg } from '../src/protocol/types';
import { FakeSocket } from './fakeSocket';

const DEV = 'dev-1';
const status = (o: Partial<CloudStatus> = {}): CloudStatus => ({
  linked: false,
  accountId: null,
  accountName: null,
  deviceId: DEV,
  expiresAt: null,
  online: true,
  error: null,
  ...o,
});
const TOKEN = 'ksd_' + 'T'.repeat(43);

type Req = { type: string; token?: string; replace?: boolean };
/** Fake client: `handle` scripts replies to requests; `emit` pushes engine events. */
function fakeClient(handle: (m: Req) => EngineMsg | Error) {
  let all: ((m: EngineMsg) => void) | null = null;
  const sent: Req[] = [];
  return {
    status: 'open' as const,
    sent,
    on: (_t: string, h: (m: EngineMsg) => void) => {
      all = h;
      return () => {};
    },
    onStatus: () => () => {},
    request: vi.fn(async (m: Req) => {
      sent.push(m);
      const r = handle(m);
      if (r instanceof Error) throw r;
      all?.(r); // replies are also dispatched to '*' handlers (as in EngineClient)
      return r;
    }),
    emit: (m: EngineMsg) => all?.(m),
  };
}
const flush = () => new Promise((r) => setTimeout(r, 0));
const until = async (cond: () => boolean, ms = 1000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timeout');
    await new Promise((r) => setTimeout(r, 5));
  }
};
const hello = (c: CloudStatus) =>
  ({ type: 'state', id: 1, patch: { layers: [] }, presetPath: null, dirty: false, audio: {}, transport: {}, version: '0.1.0', trust: 'web', cloud: c }) as unknown as EngineMsg;

afterEach(() => vi.restoreAllMocks());

function setup(
  account: { id: string; name: string },
  engineAccount: () => CloudStatus,
  opts: { storage?: Storage; exp?: string } = {},
) {
  const exp = opts.exp ?? '2027-04-10T12:00:00Z';
  let cur = engineAccount();
  const client = fakeClient((m) => {
    if (m.type === 'cloud_link') {
      cur = status({ linked: true, accountId: account.id, accountName: account.name, expiresAt: exp });
      return { type: 'cloud_status', id: 7, ...cur } as EngineMsg;
    }
    return new Error('unexpected ' + m.type);
  });
  const mint = vi.fn(async () => ({ token: TOKEN, expiresAt: exp, accountId: account.id, accountName: account.name }));
  const onProfileChanged = vi.fn();
  const store = createCloudStore({
    client: client as never,
    mode: 'platform',
    api: { mint, listDevices: async () => [] },
    storage: opts.storage ?? null,
    onProfileChanged,
    now: () => Date.parse('2026-10-12T12:00:00Z'),
  });
  return { client, store, mint, onProfileChanged, cur: () => cur };
}

describe('cloud store edge cases', () => {
  it('token dropped from another account: the page asks before linking its own account', async () => {
    const r = setup({ id: '17', name: 'Max' }, () => status({ linked: true, accountId: '42', accountName: 'Other', expiresAt: '2027-01-01T00:00:00Z' }));
    r.client.emit(hello(r.cur()));
    r.store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    await until(() => r.store.getState().link.owner === 'foreign');
    // engine drops Other's token (401)
    r.client.emit({ type: 'cloud_status', ...status({ error: 'token_expired' }) } as EngineMsg);
    await until(() => r.store.getState().link.confirm !== null);
    expect(r.store.getState().link.confirm).toEqual({ current: 'Other' });
    expect(r.client.sent.some((m) => m.type === 'cloud_link')).toBe(false); // token not handed over
    r.store.getState().dismissConfirm();
    await flush();
    expect(r.store.getState().link.confirm).toBeNull(); // not asked again in a loop
    await r.store.getState().retryLink(); // the user asks again → confirmation, then yes
    await until(() => r.store.getState().link.confirm !== null);
    await r.store.getState().confirmReplace();
    await until(() => !!r.store.getState().cloud?.linked);
    expect(r.store.getState().cloud?.accountId).toBe('17');
  });

  it('token dropped from our own account: silent re-link', async () => {
    const r = setup({ id: '17', name: 'Max' }, () => status());
    r.client.emit(hello(r.cur()));
    r.store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    await until(() => r.store.getState().link.owner === 'mine');
    r.client.emit({ type: 'cloud_status', ...status({ error: 'token_expired' }) } as EngineMsg);
    await until(() => r.mint.mock.calls.length === 2 && !!r.store.getState().cloud?.linked);
    expect(r.store.getState().link.confirm).toBeNull();
  });

  it('owner is reset when another tab re-links the engine to a different account', async () => {
    const r = setup({ id: '17', name: 'Max' }, () => status());
    r.client.emit(hello(r.cur()));
    r.store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    await until(() => r.store.getState().link.owner === 'mine');
    r.client.emit({ type: 'cloud_status', ...status({ linked: true, accountId: '42', accountName: 'Other', expiresAt: '2027-02-02T00:00:00Z' }) } as EngineMsg);
    await until(() => r.store.getState().link.owner === 'foreign');
  });

  it('re-mints at most once per page load (clock skew must not loop)', async () => {
    // expiry always "within 30 days" from the page's point of view
    const r = setup({ id: '17', name: 'Max' }, () => status(), { exp: '2026-10-20T00:00:00Z' });
    const store = createCloudStore({
      client: r.client as never,
      mode: 'platform',
      api: {
        mint: r.mint,
        listDevices: async () => [{ deviceId: DEV, name: 'x', expiresAt: '2026-10-20T00:00:00Z' }],
      },
      storage: null,
      now: () => Date.parse('2026-10-12T12:00:00Z'),
    });
    r.client.emit(hello(status({ linked: true, accountId: '17', accountName: 'Max', expiresAt: '2026-10-20T00:00:00Z' })));
    store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    await until(() => r.mint.mock.calls.length >= 1);
    await new Promise((res) => setTimeout(res, 50));
    expect(r.mint).toHaveBeenCalledTimes(1);
  });

  it('profile switch (link / unlink) asks for fresh preset + pattern lists', async () => {
    const r = setup({ id: '17', name: 'Max' }, () => status());
    r.client.emit(hello(r.cur()));
    r.store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    await until(() => !!r.store.getState().cloud?.linked);
    expect(r.onProfileChanged).toHaveBeenCalledTimes(1);
    r.client.emit({ type: 'cloud_status', ...status() } as EngineMsg);
    expect(r.onProfileChanged).toHaveBeenCalledTimes(2);
  });

  it('the device token is never stored or logged', async () => {
    const mem = new Map<string, string>();
    const storage = {
      getItem: (k: string) => mem.get(k) ?? null,
      setItem: (k: string, v: string) => void mem.set(k, v),
      removeItem: (k: string) => void mem.delete(k),
    } as unknown as Storage;
    const logs: string[] = [];
    for (const k of ['log', 'info', 'warn', 'error', 'debug'] as const)
      vi.spyOn(console, k).mockImplementation((...a: unknown[]) => void logs.push(a.map(String).join(' ')));
    const r = setup({ id: '17', name: 'Max' }, () => status(), { storage });
    r.client.emit(hello(r.cur()));
    r.store.getState().setMe({ status: 'authenticated', granted: true, name: 'Max' });
    await until(() => !!r.store.getState().cloud?.linked);
    await r.store.getState().unlink().catch(() => undefined);
    expect(r.client.sent.find((m) => m.type === 'cloud_link')?.token).toBe(TOKEN);
    expect([...mem.values()].join()).not.toContain(TOKEN);
    expect(logs.join('\n')).not.toContain(TOKEN);
    expect(JSON.stringify(r.store.getState())).not.toContain(TOKEN);
  });
});

describe('EngineClient strictId', () => {
  it('a broadcast of the expected type does not resolve a strict request', async () => {
    let sock!: FakeSocket;
    const client = new EngineClient({
      url: 'ws://x',
      jitter: 0,
      createSocket: (u) => (sock = new FakeSocket(u)) as unknown as WebSocketLike,
    });
    client.connect();
    sock.open();
    const p = client.request({ type: 'cloud_link', token: TOKEN }, { expect: 'cloud_status', strictId: true });
    const id = sock.last().id as number;
    let settled = false;
    void p.then(() => (settled = true), () => (settled = true));
    sock.receive({ type: 'cloud_status', ...status({ online: false }) });
    await flush();
    expect(settled).toBe(false);
    sock.receive({ type: 'error', id, code: 'account_mismatch', message: 'x' });
    await expect(p).rejects.toMatchObject({ code: 'account_mismatch' });
    client.close();
  });
});
