// Relay controller transport (src/platform/relay.ts) with fake sockets and fake timers:
// envelope wrap/unwrap, host selection, resync on reconnect, strict ids, heartbeat, renewal, visibility, coalescing.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { EngineClient, EngineError } from '../src/protocol/client';
import { LAST_HOST_KEY, RelayLink, parseHosts, pickHost, relayUrlFor, type RelayHost } from '../src/platform/relay';
import { FakeSocket } from './fakeSocket';

const H1: RelayHost = { deviceId: 'h1', name: 'PC', online: true };
const H2: RelayHost = { deviceId: 'h2', name: 'Studio', online: true };

function memStorage(init: Record<string, string> = {}) {
  const m = new Map(Object.entries(init));
  return { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => void m.set(k, v), map: m };
}

function setup(opts: { storage?: ReturnType<typeof memStorage>; refresh?: () => Promise<unknown> } = {}) {
  const sockets: FakeSocket[] = [];
  const refresh = vi.fn(opts.refresh ?? (() => Promise.resolve()));
  const link = new RelayLink({
    url: 'wss://mvv42.ru/keysynth/api/relay',
    cid: 'cid-1',
    createSocket: (u) => {
      const s = new FakeSocket(u);
      sockets.push(s);
      return s;
    },
    refresh,
    storage: opts.storage ?? memStorage(),
    jitter: 0,
  });
  const client = new EngineClient({ createSocket: () => link.transport(), jitter: 0, backoffInitialMs: 100, requestTimeoutMs: 1000 });
  const relay = () => sockets[sockets.length - 1]!;
  /** engine messages the controller sent through the relay */
  const sentMsgs = (s = relay()) => s.sent.filter((f) => f.t === 'msg').map((f) => f.msg as Record<string, unknown>);
  return { link, client, sockets, relay, sentMsgs, refresh };
}

const flush = async () => {
  for (let i = 0; i < 5; i++) await Promise.resolve();
};

beforeEach(() => {
  vi.useFakeTimers();
  FakeSocket.all = [];
});
afterEach(() => vi.useRealTimers());

describe('pickHost (§10.14)', () => {
  it('one online → it; else the current while online; else last chosen; else none', () => {
    expect(pickHost([H1], null, null)).toBe('h1');
    expect(pickHost([H1, { ...H2, online: false }], null, 'h2')).toBe('h1');
    expect(pickHost([H1, H2], null, null)).toBeNull();
    expect(pickHost([H1, H2], null, 'h2')).toBe('h2');
    expect(pickHost([H1, H2], 'h1', 'h2')).toBe('h1');
    expect(pickHost([{ ...H1, online: false }, H2], 'h1', null)).toBe('h2');
    // the chosen host went offline and nothing else is online: keep it (it comes back, «не в сети» meanwhile)
    expect(pickHost([{ ...H1, online: false }], 'h1', null)).toBe('h1');
    expect(pickHost([{ ...H1, online: false }, H2, { deviceId: 'h3', name: 'C', online: true }], 'h1', null)).toBe('h1');
    expect(pickHost([], 'h1', 'h1')).toBeNull();
  });

  it('parses the hosts list defensively', () => {
    expect(parseHosts([{ deviceId: 'abcdef123', online: true }, { name: 'x' }, null, { deviceId: 'h2', name: 'B', online: false, version: '0.2.0' }])).toEqual([
      { deviceId: 'abcdef123', name: 'abcdef12', online: true },
      { deviceId: 'h2', name: 'B', online: false, version: '0.2.0' },
    ]);
    expect(parseHosts('nope')).toEqual([]);
  });
});

describe('relay URL', () => {
  it('wss on https, ws on http, override wins; query role=controller + cid', () => {
    expect(relayUrlFor({ protocol: 'https:', host: 'mvv42.ru' }, '/keysynth/api')).toBe('wss://mvv42.ru/keysynth/api/relay');
    expect(relayUrlFor({ protocol: 'http:', host: '127.0.0.1:5173' }, '/keysynth/api')).toBe('ws://127.0.0.1:5173/keysynth/api/relay');
    expect(relayUrlFor({ protocol: 'https:', host: 'x' }, '/a', 'ws://127.0.0.1:7442/r')).toBe('ws://127.0.0.1:7442/r');
    const { link } = setup();
    expect(link.socketUrl).toBe('wss://mvv42.ru/keysynth/api/relay?role=controller&cid=cid-1');
  });
});

describe('RelayLink + EngineClient', () => {
  it('opens the transport only after hosts arrived and a host is online; wraps and unwraps', async () => {
    const { link, client, relay, sentMsgs } = setup();
    link.start();
    client.connect();
    relay().open();
    await flush();
    expect(client.status).toBe('connecting');
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    expect(client.status).toBe('open');
    expect(link.getState()).toMatchObject({ status: 'open', hostId: 'h1' });

    const p = client.request({ type: 'list_presets' }, { expect: 'list_presets_ok' });
    expect(sentMsgs()).toEqual([{ type: 'list_presets', id: 1 }]);
    expect(relay().sent.find((f) => f.t === 'msg')).toEqual({ t: 'msg', host: 'h1', msg: { type: 'list_presets', id: 1 } });
    // another host's message is not ours
    relay().receive({ t: 'msg', host: 'h9', msg: { type: 'list_presets_ok', id: 1, presets: [{ path: 'x' }] } });
    relay().receive({ t: 'msg', host: 'h1', msg: { type: 'list_presets_ok', id: 1, presets: [] } });
    await expect(p).resolves.toMatchObject({ type: 'list_presets_ok', presets: [] });
  });

  it('no id-less reply fallback through the relay', async () => {
    const { link, client, relay } = setup();
    link.start();
    client.connect();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    let done = false;
    const p = client.request({ type: 'list_devices' }, { expect: 'devices' }).finally(() => (done = true));
    relay().receive({ t: 'msg', host: 'h1', msg: { type: 'devices', types: [], current: {}, available: [], midiInputs: [] } });
    await flush();
    expect(done).toBe(false);
    vi.advanceTimersByTime(1000);
    await expect(p).rejects.toMatchObject({ code: 'timeout' });
  });

  it('host offline → pending rejects `disconnected`; back online → new transport + hello again', async () => {
    const { link, client, relay, sentMsgs } = setup();
    const statuses: string[] = [];
    client.onStatus((s) => statuses.push(s));
    link.start();
    client.connect();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    const p = client.request({ type: 'get_catalog' });
    relay().receive({ t: 'hosts', hosts: [{ ...H1, online: false }] });
    await expect(p).rejects.toEqual(new EngineError('disconnected', 'connection lost'));
    expect(client.status).toBe('reconnecting');
    vi.advanceTimersByTime(100); // client backoff → new transport waits for the host
    await flush();
    expect(client.status).toBe('reconnecting');
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    expect(client.status).toBe('open');
    void client.request({ type: 'hello', client: 'ui', version: '1' }).catch(() => {});
    expect(sentMsgs().at(-1)).toMatchObject({ type: 'hello' });
    expect(statuses).toContain('open');
  });

  it('relay socket drop → transport closes; reconnect refreshes the session first, with backoff', async () => {
    const { link, client, relay, sockets, refresh } = setup();
    link.start();
    client.connect();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    const p = client.request({ type: 'get_catalog' });
    relay().drop();
    await expect(p).rejects.toMatchObject({ code: 'disconnected' });
    expect(link.getState().status).toBe('reconnecting');
    expect(sockets).toHaveLength(1);
    vi.advanceTimersByTime(999);
    expect(sockets).toHaveLength(1);
    vi.advanceTimersByTime(1);
    await flush();
    expect(refresh).toHaveBeenCalledTimes(1);
    expect(sockets).toHaveLength(2);
    expect(sockets[1]!.url).toContain('cid=cid-1');
    // second failure: 2 s
    sockets[1]!.drop();
    vi.advanceTimersByTime(1999);
    await flush();
    expect(sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    await flush();
    expect(sockets).toHaveLength(3);
    sockets[2]!.open();
    sockets[2]!.receive({ t: 'hosts', hosts: [H1] });
    vi.advanceTimersByTime(100);
    await flush();
    expect(client.status).toBe('open');
    expect(link.getState()).toMatchObject({ status: 'open', failures: 0 });
  });

  it('several hosts: none chosen until the user picks; the choice is remembered', async () => {
    const storage = memStorage();
    const a = setup({ storage });
    a.link.start();
    a.client.connect();
    a.relay().open();
    a.relay().receive({ t: 'hosts', hosts: [H1, H2] });
    await flush();
    expect(a.link.getState().hostId).toBeNull();
    expect(a.client.status).toBe('connecting');
    a.link.selectHost('h2');
    await flush();
    expect(a.client.status).toBe('open');
    void a.client.request({ type: 'get_catalog' }).catch(() => {});
    expect(a.relay().sent.at(-1)).toMatchObject({ t: 'msg', host: 'h2' });
    expect(storage.map.get(LAST_HOST_KEY)).toBe('h2');

    // switching hosts = transport change: pending requests reject, the client reconnects to the new host
    const p = a.client.request({ type: 'list_presets' });
    a.link.selectHost('h1');
    await expect(p).rejects.toMatchObject({ code: 'disconnected' });
    vi.advanceTimersByTime(100);
    await flush();
    expect(a.client.status).toBe('open');
    void a.client.request({ type: 'get_catalog' }).catch(() => {});
    expect(a.relay().sent.at(-1)).toMatchObject({ t: 'msg', host: 'h1' });

    // next visit: the last chosen host is picked automatically
    const b = setup({ storage: memStorage({ [LAST_HOST_KEY]: 'h2' }) });
    b.link.start();
    b.relay().open();
    b.relay().receive({ t: 'hosts', hosts: [H1, H2] });
    expect(b.link.getState().hostId).toBe('h2');
  });

  it('heartbeat: RTT from ping/pong, answers server pings, reconnects after 45 s silence', async () => {
    const { link, relay, sockets } = setup();
    link.start();
    relay().open();
    expect(relay().sent).toEqual([{ t: 'ping' }]);
    vi.advanceTimersByTime(42);
    relay().receive({ t: 'pong' });
    expect(link.getState().rtt).toBe(42);
    relay().receive({ t: 'ping' });
    expect(relay().last()).toEqual({ t: 'pong' });
    relay().receive({ t: 'hosts', hosts: [H1] });
    vi.advanceTimersByTime(20_000);
    expect(relay().sent.filter((f) => f.t === 'ping')).toHaveLength(2);
    vi.advanceTimersByTime(31_000); // nothing received for > 45 s (checked every 5 s)
    expect(link.getState().status).toBe('reconnecting');
    expect(sockets[0]!.readyState).toBe(3);
  });

  it('visibility: pause/resume; becoming visible skips the backoff', async () => {
    const { link, relay, sockets } = setup();
    link.start();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    link.setVisible(false);
    expect(relay().last()).toEqual({ t: 'pause' });
    link.setVisible(true);
    expect(relay().sent.slice(-2)).toEqual([{ t: 'resume' }, { t: 'ping' }]);
    // hidden, socket dies, backoff pending → visible reconnects right away
    link.setVisible(false);
    relay().drop();
    expect(sockets).toHaveLength(1);
    link.setVisible(true);
    await flush(); // session refresh, then the socket (no backoff)
    expect(sockets).toHaveLength(2);
    // a socket opened while hidden pauses at once
    link.setVisible(false);
    sockets[1]!.drop();
    vi.advanceTimersByTime(2000); // second failure in a row
    await flush();
    sockets[2]!.open();
    expect(sockets[2]!.sent[0]).toEqual({ t: 'pause' });
  });

  it('a frozen tab with a silent socket reconnects when it becomes visible', async () => {
    const { link, relay, sockets, refresh } = setup();
    link.start();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    link.setVisible(false);
    vi.setSystemTime(Date.now() + 60_000); // timers did not run (frozen), but the clock moved
    link.setVisible(true);
    expect(sockets[0]!.readyState).toBe(3);
    expect(link.getState().status).toBe('reconnecting');
    await flush();
    expect(refresh).toHaveBeenCalled();
    expect(sockets).toHaveLength(2); // at once, not after the backoff
  });

  it('the hub closes the old socket before the renewal even opened: the renewal is adopted', async () => {
    const { link, client, sockets, relay } = setup();
    link.start();
    client.connect();
    sockets[0]!.open();
    sockets[0]!.receive({ t: 'hosts', hosts: [H1] });
    await flush();
    for (let i = 0; i < 150; i++) {
      vi.advanceTimersByTime(20_000);
      sockets[0]!.receive({ t: 'ping' });
    }
    await flush();
    expect(sockets).toHaveLength(2);
    sockets[0]!.drop(); // 4000 before the renewal's onopen
    expect(link.getState().status).toBe('open');
    expect(client.status).toBe('open');
    expect(relay()).toBe(sockets[1]);
    sockets[1]!.open();
    const p = client.request({ type: 'get_catalog' });
    expect(sockets[1]!.sent.at(-1)).toMatchObject({ t: 'msg', msg: { type: 'get_catalog' } });
    sockets[1]!.receive({ t: 'msg', host: 'h1', msg: { type: 'catalog_ok', id: 1, modules: [] } });
    await expect(p).resolves.toMatchObject({ type: 'catalog_ok' });
    expect(sockets).toHaveLength(2);
  });

  it('close codes: 4000 (cid replaced) does not reconnect; 4029 backs off to the maximum', async () => {
    const a = setup();
    a.link.start();
    a.relay().open();
    a.relay().receive({ t: 'hosts', hosts: [H1] });
    a.relay().onclose?.({ code: 4000 });
    vi.advanceTimersByTime(60_000);
    await flush();
    expect(a.sockets).toHaveLength(1);
    a.link.retryNow(); // «Повторить» resumes
    await flush();
    expect(a.sockets).toHaveLength(2);

    const b = setup();
    b.link.start();
    b.relay().open();
    b.relay().receive({ t: 'hosts', hosts: [H1] });
    b.relay().onclose?.({ code: 4029 });
    vi.advanceTimersByTime(29_999);
    await flush();
    expect(b.sockets).toHaveLength(1);
    vi.advanceTimersByTime(1);
    await flush();
    expect(b.sockets).toHaveLength(2);
  });

  it('a socket stuck connecting is abandoned after 10 s', async () => {
    const { link, sockets } = setup();
    link.start();
    vi.advanceTimersByTime(10_000);
    expect(sockets[0]!.readyState).toBe(3);
    expect(link.getState().status).toBe('reconnecting');
    vi.advanceTimersByTime(1000);
    await flush();
    expect(sockets).toHaveLength(2);
  });

  it('renewal at ~50 min: refresh, same cid, make-before-break without touching the transport', async () => {
    const { link, client, relay, sockets, refresh } = setup();
    link.start();
    client.connect();
    sockets[0]!.open();
    sockets[0]!.receive({ t: 'hosts', hosts: [H1] });
    await flush();
    const statuses: string[] = [];
    client.onStatus((s) => statuses.push(s));
    // keep the first socket alive (server pings)
    for (let i = 0; i < 150; i++) {
      vi.advanceTimersByTime(20_000);
      sockets[0]!.receive({ t: 'ping' });
    }
    await flush();
    expect(refresh).toHaveBeenCalledTimes(1);
    expect(sockets).toHaveLength(2);
    expect(sockets[1]!.url).toBe(sockets[0]!.url);
    sockets[1]!.open();
    expect(sockets[0]!.readyState).toBe(1); // old stays until the new one delivered hosts
    const p = client.request({ type: 'get_catalog' });
    expect(sockets[0]!.sent.at(-1)).toMatchObject({ t: 'msg' });
    sockets[1]!.receive({ t: 'hosts', hosts: [H1] });
    expect(sockets[0]!.readyState).toBe(3);
    expect(relay()).toBe(sockets[1]);
    // the reply may come on the new socket (server resumed the cid)
    sockets[1]!.receive({ t: 'msg', host: 'h1', msg: { type: 'catalog_ok', id: 1, modules: [] } });
    await expect(p).resolves.toMatchObject({ type: 'catalog_ok' });
    void client.request({ type: 'list_presets' }).catch(() => {});
    expect(sockets[1]!.sent.at(-1)).toMatchObject({ t: 'msg', msg: { type: 'list_presets' } });
    expect(statuses).toEqual([]);
    expect(client.status).toBe('open');
  });

  it('server closes the old socket before the renewal delivered hosts: the renewal takes over', async () => {
    const { link, client, sockets } = setup();
    link.start();
    client.connect();
    sockets[0]!.open();
    sockets[0]!.receive({ t: 'hosts', hosts: [H1] });
    await flush();
    for (let i = 0; i < 150; i++) {
      vi.advanceTimersByTime(20_000);
      sockets[0]!.receive({ t: 'ping' });
    }
    await flush();
    sockets[1]!.open();
    sockets[0]!.drop(); // 4000 resumed elsewhere
    expect(link.getState().status).toBe('open');
    expect(client.status).toBe('open');
    void client.request({ type: 'list_presets' }).catch(() => {});
    expect(sockets[1]!.sent.at(-1)).toMatchObject({ t: 'msg' });
  });

  it('coalesces set_param to ≤ 30 Hz (latest wins) and keeps order with other messages', async () => {
    const { link, client, relay, sentMsgs } = setup();
    link.start();
    client.connect();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    for (let v = 1; v <= 5; v++) client.send({ type: 'set_param', node: 3, param: 'cutoff', value: v });
    client.send({ type: 'set_param', node: 4, param: 'gain', value: 0.5 });
    expect(sentMsgs()).toEqual([{ type: 'set_param', node: 3, param: 'cutoff', value: 1 }]);
    vi.advanceTimersByTime(32);
    expect(sentMsgs()).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(sentMsgs().slice(1)).toEqual([
      { type: 'set_param', node: 3, param: 'cutoff', value: 5 },
      { type: 'set_param', node: 4, param: 'gain', value: 0.5 },
    ]);
    // a pending param goes out before a later structural message
    client.send({ type: 'set_param', node: 3, param: 'cutoff', value: 6 });
    client.send({ type: 'panic' });
    expect(sentMsgs().slice(3)).toEqual([{ type: 'set_param', node: 3, param: 'cutoff', value: 6 }, { type: 'panic' }]);
    // 60 updates over 1 s → about 30 frames
    const before = sentMsgs().length;
    for (let i = 0; i < 60; i++) {
      client.send({ type: 'set_param', node: 3, param: 'cutoff', value: i });
      vi.advanceTimersByTime(1000 / 60);
    }
    const n = sentMsgs().length - before;
    expect(n).toBeGreaterThanOrEqual(28);
    expect(n).toBeLessThanOrEqual(31);
    expect(sentMsgs().at(-1)).toMatchObject({ value: 59 });
  });

  it('host_offline with the request id rejects that request at once', async () => {
    const { link, client, relay } = setup();
    link.start();
    client.connect();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    await flush();
    const p = client.request({ type: 'get_catalog' });
    relay().receive({ t: 'error', code: 'host_offline', host: 'h1', id: 1 });
    await expect(p).rejects.toMatchObject({ code: 'host_offline' });
    expect(client.status).toBe('open');
  });

  it('stop() closes everything; error frames are kept for the UI', async () => {
    const { link, client, relay } = setup();
    link.start();
    client.connect();
    relay().open();
    relay().receive({ t: 'hosts', hosts: [H1] });
    relay().receive({ t: 'error', code: 'host_offline' });
    expect(link.getState().error).toBe('host_offline');
    await flush();
    link.stop();
    expect(relay().readyState).toBe(3);
    expect(link.getState()).toMatchObject({ status: 'idle', hosts: [], hostId: null });
    expect(client.status).toBe('reconnecting');
  });
});

describe('EngineClient.switchTransport', () => {
  it('rejects pending with `disconnected` and connects at once (or stays closed)', async () => {
    const c = new EngineClient({ createSocket: (u) => new FakeSocket(u), jitter: 0 });
    c.connect();
    FakeSocket.all[0]!.open();
    const p = c.request({ type: 'get_catalog' });
    c.switchTransport(true);
    await expect(p).rejects.toEqual(new EngineError('disconnected', 'transport changed'));
    expect(FakeSocket.all).toHaveLength(2);
    expect(c.status).toBe('connecting');
    c.switchTransport(false);
    expect(c.status).toBe('closed');
    expect(FakeSocket.all).toHaveLength(2);
  });
});
