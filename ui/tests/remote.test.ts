// Route policy, remote trust, the route controller (store/remoteStore.ts), Пульт helpers.
import { describe, expect, it, vi } from 'vitest';
import { createStore } from 'zustand/vanilla';
import { decideRoute, type RouteInput } from '../src/platform/route';
import { canSend } from '../src/platform/trust';
import { planLink } from '../src/platform/link';
import { createRemote } from '../src/store/remoteStore';
import type { CloudState } from '../src/store/cloudStore';
import type { RelayLink } from '../src/platform/relay';
import { defaultView } from '../src/lib/view';
import { pushRecent } from '../src/lib/recent';
import { visiblePresets } from '../src/lib/presetTiles';
import type { PresetEntry } from '../src/protocol/types';
import { FakeSocket } from './fakeSocket';

const signedIn = { status: 'authenticated' as const, granted: true };
const fresh = { seen: false, started: false };
const input = (o: Partial<RouteInput> = {}): RouteInput => ({ mode: 'platform', me: signedIn, discovery: fresh, ...o });

describe('decideRoute (§6, §10.12)', () => {
  it('relay only on the platform page, signed in with access, without a local engine', () => {
    expect(decideRoute(input())).toBe('relay');
    expect(decideRoute(input({ mode: 'local' }))).toBe('direct');
    expect(decideRoute(input({ discovery: { seen: true, started: true } }))).toBe('direct');
    expect(decideRoute(input({ discovery: { seen: false, started: true } }))).toBe('direct'); // «Движок на этом ПК»
    expect(decideRoute(input({ me: { status: 'anonymous', granted: false } }))).toBe('direct');
    expect(decideRoute(input({ me: { status: 'authenticated', granted: false } }))).toBe('direct');
    expect(decideRoute(input({ me: { status: 'loading', granted: false } }))).toBe('direct');
    expect(decideRoute(input({ me: { status: 'unavailable', granted: false } }))).toBe('direct');
    expect(decideRoute(input({ force: 'relay', discovery: { seen: true, started: true } }))).toBe('relay');
    expect(decideRoute(input({ force: 'direct' }))).toBe('direct');
  });
});

describe('remote trust (§10.3)', () => {
  it('hides what the engine refuses for remote sessions', () => {
    for (const t of ['cloud_link', 'cloud_unlink', 'set_audio_device', 'open_audio_panel', 'samples_delete', 'reload_plugin'])
      expect(canSend('remote', t)).toBe(false);
    for (const t of ['load_preset', 'set_param', 'transport', 'panic', 'restart_audio', 'samples_fetch', 'sync_now', 'save_preset'])
      expect(canSend('remote', t)).toBe(true);
    expect(canSend('web', 'reload_plugin')).toBe(false);
    expect(canSend('web', 'set_audio_device')).toBe(true);
    expect(canSend('local', 'reload_plugin')).toBe(true);
    expect(canSend(null, 'cloud_link')).toBe(true);
  });

  it('a remote session never plans a link', () => {
    const cloud = { linked: true, accountId: '17', accountName: 'Max', deviceId: 'd', expiresAt: '2026-10-20T00:00:00Z', online: true, error: null };
    const base = { mode: 'platform' as const, connected: true, cloud, meStatus: 'authenticated' as const, granted: true, owner: 'unknown' as const, now: Date.parse('2026-10-13T00:00:00Z') };
    expect(planLink(base).kind).toBe('check-owner');
    expect(planLink({ ...base, remote: true })).toEqual({ kind: 'idle', reason: 'remote' });
    expect(planLink({ ...base, remote: true, cloud: { ...cloud, linked: false } })).toEqual({ kind: 'idle', reason: 'remote' });
  });
});

function fakeCloud(o: { me?: CloudState['me']; discovery?: Partial<CloudState['discovery']> } = {}) {
  return createStore<CloudState>()(
    () =>
      ({
        me: o.me ?? { status: 'loading', granted: false, name: null },
        discovery: { phase: 'never-seen', seen: false, started: false, permission: 'unknown', failures: 0, ...o.discovery },
      }) as CloudState,
  );
}

function fakeRelay() {
  let listener: (() => void) | null = null;
  const state = { status: 'idle' as const, hosts: [], hostId: null, rtt: null, error: null, failures: 0 };
  return {
    start: vi.fn(),
    stop: vi.fn(),
    retryNow: vi.fn(),
    selectHost: vi.fn(),
    setVisible: vi.fn(),
    transport: vi.fn(() => new FakeSocket('relay')),
    getState: () => state,
    subscribe: (fn: () => void) => {
      listener = fn;
      return () => (listener = null);
    },
    get listener() {
      return listener;
    },
  };
}

describe('route controller', () => {
  it('signs in → relay (transport switched, relay started); local engine asked → direct; sign out → direct', () => {
    const cloud = fakeCloud();
    const relay = fakeRelay();
    const client = { switchTransport: vi.fn(), retryNow: vi.fn() };
    const direct = vi.fn((u: string) => new FakeSocket(u));
    const r = createRemote({ client, cloud, relay: relay as unknown as RelayLink, mode: 'platform', direct });
    expect(r.store.getState().route).toBe('direct');
    expect(client.switchTransport).not.toHaveBeenCalled();
    r.createSocket('ws://127.0.0.1:7341');
    expect(direct).toHaveBeenCalledTimes(1);

    cloud.setState({ me: { status: 'authenticated', granted: true, name: 'Max' } });
    expect(r.store.getState().route).toBe('relay');
    expect(relay.start).toHaveBeenCalled();
    expect(client.switchTransport).toHaveBeenLastCalledWith(true);
    expect(r.isDirect()).toBe(false);
    r.createSocket('ws://127.0.0.1:7341');
    expect(relay.transport).toHaveBeenCalledTimes(1);
    expect(direct).toHaveBeenCalledTimes(1);

    // «Движок на этом ПК»: discovery started → direct, connect
    cloud.setState({ discovery: { ...cloud.getState().discovery, started: true } });
    expect(r.store.getState().route).toBe('direct');
    expect(relay.stop).toHaveBeenCalled();
    expect(client.switchTransport).toHaveBeenLastCalledWith(true);
  });

  it('signing out on the relay route goes direct without opening a local socket', () => {
    const cloud = fakeCloud({ me: { status: 'authenticated', granted: true, name: 'Max' } });
    const relay = fakeRelay();
    const client = { switchTransport: vi.fn(), retryNow: vi.fn() };
    const r = createRemote({ client, cloud, relay: relay as unknown as RelayLink, mode: 'platform', direct: (u) => new FakeSocket(u) });
    expect(r.store.getState().route).toBe('relay');
    cloud.setState({ me: { status: 'anonymous', granted: false, name: null } });
    expect(r.store.getState().route).toBe('direct');
    expect(client.switchTransport).toHaveBeenLastCalledWith(false);
  });

  it('visibility reaches the relay; visible on the relay route retries the client at once', () => {
    const cloud = fakeCloud({ me: { status: 'authenticated', granted: true, name: 'Max' } });
    const relay = fakeRelay();
    const client = { switchTransport: vi.fn(), retryNow: vi.fn() };
    let handler: (() => void) | null = null;
    const doc = { visibilityState: 'visible', addEventListener: (_: string, fn: () => void) => (handler = fn) };
    createRemote({ client, cloud, relay: relay as unknown as RelayLink, mode: 'platform', direct: (u) => new FakeSocket(u), document: doc });
    expect(relay.setVisible).toHaveBeenLastCalledWith(true);
    doc.visibilityState = 'hidden';
    handler!();
    expect(relay.setVisible).toHaveBeenLastCalledWith(false);
    expect(client.retryNow).not.toHaveBeenCalled();
    doc.visibilityState = 'visible';
    handler!();
    expect(relay.setVisible).toHaveBeenLastCalledWith(true);
    expect(client.retryNow).toHaveBeenCalledTimes(1);
  });

  it('without a relay (local mode) the route is always direct', () => {
    const cloud = fakeCloud({ me: { status: 'authenticated', granted: true, name: 'Max' } });
    const client = { switchTransport: vi.fn(), retryNow: vi.fn() };
    const r = createRemote({ client, cloud, relay: null, mode: 'local', direct: (u) => new FakeSocket(u) });
    expect(r.store.getState().route).toBe('direct');
    expect(client.switchTransport).not.toHaveBeenCalled();
  });
});

describe('Пульт helpers', () => {
  it('default view: stored choice, else Пульт on touch tablets', () => {
    expect(defaultView({ stored: null, coarse: true, width: 820 })).toBe('pult');
    expect(defaultView({ stored: null, coarse: true, width: 600 })).toBe('editor');
    expect(defaultView({ stored: null, coarse: false, width: 1440 })).toBe('editor');
    expect(defaultView({ stored: 'editor', coarse: true, width: 1180 })).toBe('editor');
    expect(defaultView({ stored: 'pult', coarse: false, width: 1440 })).toBe('pult');
    expect(defaultView({ stored: 'junk', coarse: false, width: 1440 })).toBe('editor');
  });

  it('recent: newest first, deduplicated, capped', () => {
    expect(pushRecent(['a', 'b', 'c'], 'b')).toEqual(['b', 'a', 'c']);
    expect(pushRecent(['a', 'b'], 'c', 2)).toEqual(['c', 'a']);
  });

  it('tiles: favourites, recent order, search over name/category/tags', () => {
    const p = (path: string, name: string, category = 'Keys', tags: string[] = []): PresetEntry => ({ path, name, category, tags, factory: true });
    const all = [p('a', 'Grand'), p('b', 'Rhodes', 'EP', ['vintage']), p('c', 'Pad', 'Synth')];
    expect(visiblePresets(all, 'all', '', new Set(), []).map((x) => x.path)).toEqual(['a', 'b', 'c']);
    expect(visiblePresets(all, 'fav', '', new Set(['c']), []).map((x) => x.path)).toEqual(['c']);
    expect(visiblePresets(all, 'recent', '', new Set(), ['c', 'gone', 'a']).map((x) => x.path)).toEqual(['c', 'a']);
    expect(visiblePresets(all, 'fav', 'VINT', new Set(), []).map((x) => x.path)).toEqual(['b']);
    expect(visiblePresets(all, 'all', ' synth ', new Set(), []).map((x) => x.path)).toEqual(['c']);
  });
});
