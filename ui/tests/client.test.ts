import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { EngineClient, EngineError, engineUrlFromLocation } from '../src/protocol/client';
import { FakeSocket, latestSocket } from './fakeSocket';

function makeClient(extra: Partial<ConstructorParameters<typeof EngineClient>[0]> = {}) {
  return new EngineClient({
    url: 'ws://test',
    createSocket: (u) => new FakeSocket(u),
    jitter: 0,
    backoffInitialMs: 100,
    backoffMaxMs: 1000,
    requestTimeoutMs: 500,
    ...extra,
  });
}

beforeEach(() => {
  vi.useFakeTimers();
  FakeSocket.all = [];
});
afterEach(() => vi.useRealTimers());

describe('EngineClient request/response', () => {
  it('correlates replies by id', async () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const p1 = c.request({ type: 'list_presets' });
    const p2 = c.request({ type: 'get_catalog' });
    const [m1, m2] = latestSocket().sent;
    expect(m1).toMatchObject({ type: 'list_presets', id: 1 });
    expect(m2).toMatchObject({ type: 'get_catalog', id: 2 });
    latestSocket().receive({ type: 'catalog_ok', id: 2, modules: [] });
    latestSocket().receive({ type: 'list_presets_ok', id: 1, presets: [] });
    await expect(p2).resolves.toMatchObject({ type: 'catalog_ok' });
    await expect(p1).resolves.toMatchObject({ type: 'list_presets_ok' });
  });

  it('request ids are monotonic and never reused, also across reconnects', async () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    void c.request({ type: 'list_presets' }).catch(() => {});
    void c.request({ type: 'list_presets' }).catch(() => {});
    latestSocket().drop();
    vi.advanceTimersByTime(100);
    latestSocket().open();
    void c.request({ type: 'list_presets' }).catch(() => {});
    const ids = FakeSocket.all.flatMap((s) => s.sent.map((m) => m.id));
    expect(ids).toEqual([1, 2, 3]);
  });

  it('rejects with EngineError on error reply', async () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const p = c.request({ type: 'load_preset', path: 'x' });
    latestSocket().receive({ type: 'error', id: 1, code: 'not_found', message: 'nope' });
    await expect(p).rejects.toMatchObject({ code: 'not_found', message: 'nope' });
  });

  it('times out', async () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const p = c.request({ type: 'list_presets' });
    vi.advanceTimersByTime(501);
    await expect(p).rejects.toBeInstanceOf(EngineError);
    await expect(p).rejects.toMatchObject({ code: 'timeout' });
  });

  it('falls back to `expect` when the reply has no id', async () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const p = c.request({ type: 'hello', client: 'ui', version: '1' }, { expect: 'state' });
    latestSocket().receive({ type: 'telemetry', cpu: 0, xruns: 0, voices: 0, meters: { master: [0, 0], layers: {} }, readouts: {} });
    latestSocket().receive({ type: 'state', patch: {}, presetPath: null, dirty: false, audio: {}, transport: {} });
    await expect(p).resolves.toMatchObject({ type: 'state' });
  });

  it('rejects immediately when disconnected; send() returns false', async () => {
    const c = makeClient();
    await expect(c.request({ type: 'list_presets' })).rejects.toMatchObject({ code: 'disconnected' });
    expect(c.send({ type: 'panic' })).toBe(false);
  });

  it('fails pending requests when the socket drops', async () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const p = c.request({ type: 'list_presets' });
    latestSocket().drop();
    await expect(p).rejects.toMatchObject({ code: 'disconnected' });
  });
});

describe('EngineClient events', () => {
  it('dispatches by type and to wildcard; unsubscribe works', () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const onMidi = vi.fn();
    const onAll = vi.fn();
    const off = c.on('midi', onMidi);
    c.on('*', onAll);
    latestSocket().receive({ type: 'midi', notes: [{ note: 60, on: true, velocity: 100 }] });
    latestSocket().receive({ type: 'log', level: 'info', message: 'x' });
    expect(onMidi).toHaveBeenCalledTimes(1);
    expect(onAll).toHaveBeenCalledTimes(2);
    off();
    latestSocket().receive({ type: 'midi', notes: [] });
    expect(onMidi).toHaveBeenCalledTimes(1);
  });

  it('ignores malformed frames', () => {
    const c = makeClient();
    c.connect();
    latestSocket().open();
    const onAll = vi.fn();
    c.on('*', onAll);
    latestSocket().onmessage?.({ data: 'not json' });
    latestSocket().onmessage?.({ data: '{"no":"type"}' });
    expect(onAll).not.toHaveBeenCalled();
  });
});

describe('EngineClient reconnect', () => {
  it('reconnects with exponential backoff capped at max, resets after open', () => {
    const c = makeClient();
    const statuses: string[] = [];
    c.onStatus((s) => statuses.push(s));
    c.connect();
    expect(FakeSocket.all).toHaveLength(1);
    latestSocket().drop(); // attempt 1 → 100 ms
    vi.advanceTimersByTime(99);
    expect(FakeSocket.all).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(FakeSocket.all).toHaveLength(2);
    latestSocket().drop(); // attempt 2 → 200 ms
    vi.advanceTimersByTime(199);
    expect(FakeSocket.all).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(FakeSocket.all).toHaveLength(3);
    expect(c.backoffDelay(3)).toBe(400);
    expect(c.backoffDelay(10)).toBe(1000);
    latestSocket().open();
    expect(c.status).toBe('open');
    latestSocket().drop(); // attempt counter reset → 100 ms again
    vi.advanceTimersByTime(100);
    expect(FakeSocket.all).toHaveLength(4);
    expect(statuses).toContain('reconnecting');
    expect(statuses).toContain('open');
  });

  it('close() stops reconnecting', () => {
    const c = makeClient();
    c.connect();
    latestSocket().drop();
    c.close();
    vi.advanceTimersByTime(10000);
    expect(FakeSocket.all).toHaveLength(1);
    expect(c.status).toBe('closed');
  });
});

describe('engineUrlFromLocation', () => {
  it('parses ?engine=', () => {
    expect(engineUrlFromLocation('')).toBe('ws://127.0.0.1:7341');
    expect(engineUrlFromLocation('?engine=127.0.0.1:7400')).toBe('ws://127.0.0.1:7400');
    expect(engineUrlFromLocation('?engine=ws://host:1/x')).toBe('ws://host:1/x');
  });
});
