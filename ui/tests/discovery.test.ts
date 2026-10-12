import { describe, expect, it, vi } from 'vitest';
import {
  ENGINE_SEEN_KEY,
  createDiscovery,
  initialDiscovery,
  reduceDiscovery,
  shouldAutoProbe,
  watchLnaPermission,
  type DiscoveryEvent,
  type DiscoveryState,
  type PermissionsLike,
} from '../src/platform/discovery';
import { detectMode } from '../src/platform/mode';
import type { ConnectionStatus } from '../src/protocol/client';

const run = (s: DiscoveryState, ...events: DiscoveryEvent[]) => events.reduce(reduceDiscovery, s);
const sock = (status: ConnectionStatus): DiscoveryEvent => ({ type: 'socket', status });

describe('discovery state machine', () => {
  it('never-seen waits for the user (no auto-probe before the engine was seen)', () => {
    const s = initialDiscovery(false);
    expect(s.phase).toBe('never-seen');
    expect(s.started).toBe(false);
    expect(shouldAutoProbe(s)).toBe(false);
  });

  it('auto-probes when the engine was seen before or the permission is granted', () => {
    expect(initialDiscovery(true)).toMatchObject({ phase: 'connecting', started: true });
    expect(initialDiscovery(false, 'granted')).toMatchObject({ phase: 'connecting', started: true });
    expect(shouldAutoProbe(initialDiscovery(false, 'prompt'))).toBe(false);
  });

  it('click → connecting → connected marks the engine as seen', () => {
    const s = run(initialDiscovery(false), { type: 'start' }, sock('connecting'), sock('open'));
    expect(s).toMatchObject({ phase: 'connected', seen: true, failures: 0 });
  });

  it('stays «connecting» on the first failure, then «not-running» while retrying', () => {
    let s = run(initialDiscovery(false), { type: 'start' }, sock('connecting'), sock('reconnecting'));
    expect(s.phase).toBe('connecting');
    s = run(s, sock('reconnecting'));
    expect(s.phase).toBe('not-running');
    s = run(s, sock('reconnecting'), sock('reconnecting'));
    expect(s.phase).toBe('not-running');
    s = run(s, sock('open'));
    expect(s.phase).toBe('connected');
  });

  it('a lost connection goes connecting → not-running and back', () => {
    let s = run(initialDiscovery(true), sock('open'));
    s = run(s, sock('reconnecting'));
    expect(s.phase).toBe('connecting');
    s = run(s, sock('reconnecting'));
    expect(s.phase).toBe('not-running');
    expect(run(s, sock('open')).phase).toBe('connected');
  });

  it('denied permission is shown once an attempt failed (query is only a hint); granting it starts discovery', () => {
    let s = run(initialDiscovery(false), { type: 'permission', state: 'denied' });
    expect(s.phase).toBe('never-seen');
    s = run(s, { type: 'start' });
    expect(s.phase).toBe('connecting');
    expect(run(s, sock('open')).phase).toBe('connected'); // reported denied, but the socket works → connected
    s = run(s, sock('reconnecting'));
    expect(s.phase).toBe('permission-denied');
    s = run(s, sock('reconnecting'));
    expect(s.phase).toBe('permission-denied');
    s = run(s, { type: 'permission', state: 'granted' });
    expect(s.started).toBe(true);
    expect(s.phase).toBe('not-running'); // failures so far still count until the socket opens
    expect(run(s, sock('open')).phase).toBe('connected');
  });

  it('a permission change while connected does not drop the connected phase', () => {
    const s = run(initialDiscovery(true), sock('open'), { type: 'permission', state: 'prompt' });
    expect(s.phase).toBe('connected');
  });

  it('«retry» resets the failure count', () => {
    const s = run(initialDiscovery(false), { type: 'start' }, sock('reconnecting'), sock('reconnecting'), { type: 'start' });
    expect(s).toMatchObject({ phase: 'connecting', failures: 0 });
  });
});

describe('discovery glue', () => {
  const fakeClient = () => {
    let handler: ((s: ConnectionStatus, a: number) => void) | null = null;
    return {
      connect: vi.fn(),
      retryNow: vi.fn(),
      onStatus: (h: (s: ConnectionStatus, a: number) => void) => {
        handler = h;
        return () => (handler = null);
      },
      emit: (s: ConnectionStatus) => handler?.(s, 0),
    };
  };
  const memStorage = (init: Record<string, string> = {}) => {
    const m = new Map(Object.entries(init));
    return { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => void m.set(k, v), m };
  };

  it('does not open a socket before the click; remembers the engine after it answered', async () => {
    const client = fakeClient();
    const storage = memStorage();
    const states: string[] = [];
    const d = createDiscovery({ client, storage, onState: (s) => states.push(s.phase) });
    await Promise.resolve();
    expect(client.connect).not.toHaveBeenCalled();
    expect(d.state.phase).toBe('never-seen');
    d.start();
    expect(client.connect).toHaveBeenCalledTimes(1);
    client.emit('connecting');
    client.emit('open');
    expect(d.state.phase).toBe('connected');
    expect(storage.m.get(ENGINE_SEEN_KEY)).toBe('1');
    d.start(); // already started → immediate retry instead of a second connect()
    expect(client.retryNow).toHaveBeenCalledTimes(1);
    expect(states).toContain('connecting');
  });

  it('auto-connects when the engine was seen before', () => {
    const client = fakeClient();
    createDiscovery({ client, storage: memStorage({ [ENGINE_SEEN_KEY]: '1' }), onState: () => {} });
    expect(client.connect).toHaveBeenCalledTimes(1);
  });

  it('auto-connects once the LNA permission reports granted', async () => {
    const client = fakeClient();
    const perms: PermissionsLike = {
      query: async ({ name }) => {
        if (name !== 'local-network-access') throw new TypeError('unknown permission');
        return { state: 'granted', onchange: null };
      },
    };
    const d = createDiscovery({ client, storage: memStorage(), permissions: perms, onState: () => {} });
    await new Promise((r) => setTimeout(r, 0));
    expect(d.state.permission).toBe('granted');
    expect(client.connect).toHaveBeenCalledTimes(1);
  });

  it('watchLnaPermission: unknown names → unknown; onchange forwards updates', async () => {
    expect(await watchLnaPermission(undefined, () => {})).toBe('unknown');
    expect(await watchLnaPermission({ query: async () => Promise.reject(new TypeError('x')) }, () => {})).toBe('unknown');
    const st = { state: 'prompt', onchange: null as null | ((this: unknown, ev: unknown) => unknown) };
    const seen: string[] = [];
    expect(await watchLnaPermission({ query: async () => st }, (s) => seen.push(s))).toBe('prompt');
    st.state = 'denied';
    st.onchange?.call(st, {});
    expect(seen).toEqual(['denied']);
  });
});

describe('mode detection', () => {
  const loc = (hostname: string, port = '', search = '') => ({ hostname, port, search });
  it('mvv42.ru → platform, engine always ws://127.0.0.1:7341 (no ?engine= in production)', () => {
    expect(detectMode(loc('mvv42.ru', '', '?engine=evil.example:1'), true, false)).toEqual({
      mode: 'platform',
      engineUrl: 'ws://127.0.0.1:7341',
      dev: false,
    });
  });
  it('served by the engine at :7340 → local, whatever the build', () => {
    expect(detectMode(loc('127.0.0.1', '7340'), true, false).mode).toBe('local');
    expect(detectMode(loc('localhost', '7340'), false, false).mode).toBe('local');
  });
  it('plain dev server → local; platform dev build → platform; dev honours ?engine=', () => {
    expect(detectMode(loc('127.0.0.1', '5173'), false, true).mode).toBe('local');
    const m = detectMode(loc('127.0.0.1', '5173', '?engine=127.0.0.1:7999'), true, true);
    expect(m).toEqual({ mode: 'platform', engineUrl: 'ws://127.0.0.1:7999', dev: true });
  });
  it('dev server default engine (npm run dev:mock → the mock port); ?engine= wins; ignored in production', () => {
    const mock = 'ws://127.0.0.1:7441';
    expect(detectMode(loc('127.0.0.1', '5173'), false, true, mock).engineUrl).toBe(mock);
    expect(detectMode(loc('127.0.0.1', '5173', '?engine=127.0.0.1:7351'), false, true, mock).engineUrl).toBe('ws://127.0.0.1:7351');
    expect(detectMode(loc('mvv42.ru'), true, false, mock).engineUrl).toBe('ws://127.0.0.1:7341');
    expect(detectMode(loc('127.0.0.1', '7340'), false, false, mock).engineUrl).toBe('ws://127.0.0.1:7341');
  });
});
