import { useStore } from 'zustand';
import { createStore } from 'zustand/vanilla';
import { getMe } from '@mvv42/kit/platform';
import { EngineClient, type WebSocketLike } from '../protocol/client';
import { bindClient, createEngineStore, type EngineState } from './engineStore';
import { createCloudStore, type CloudState } from './cloudStore';
import { createSongStore, type SongsState } from './songStore';
import { IDLE_RELAY, createRemote, type Remote, type RemoteState } from './remoteStore';
import { API_BASE, MODE } from '../platform/mode';
import { createDiscovery, type PermissionsLike } from '../platform/discovery';
import { RelayLink, relayUrlFor } from '../platform/relay';
import type { Route } from '../platform/route';
import { listDevices, mintDevice } from '../platform/api';
import { fetchLatestRelease } from '../platform/version';
import { noteRecent } from '../lib/recent';

const browser = typeof window !== 'undefined';
const safeLocalStorage = (() => {
  try {
    return browser ? window.localStorage : null;
  } catch {
    return null;
  }
})();

const directSocket = (url: string) => new WebSocket(url) as unknown as WebSocketLike;
/** set below; the socket factory asks it whether the route is the relay */
let remote: Remote | null = null;

// The platform page probes less eagerly (LNA prompt): back off up to 10 s.
export const client = new EngineClient({
  url: MODE.engineUrl,
  ...(MODE.mode === 'platform' ? { backoffMaxMs: 10000 } : {}),
  createSocket: (url) => (remote ? remote.createSocket(url) : directSocket(url)),
});
export const engineStore = createEngineStore({ client });
bindClient(engineStore, client, { acceptMock: MODE.dev });
export const songStore = createSongStore({ client, acceptMock: MODE.dev });

export const cloudStore = createCloudStore({
  client,
  mode: MODE.mode,
  acceptMock: MODE.dev,
  api: { mint: mintDevice, listDevices },
  storage: safeLocalStorage,
  isRemote: () => remote?.isDirect() === false,
  onProfileChanged: () => {
    void engineStore.getState().refreshPresets();
    void engineStore.getState().refreshPatterns();
    void songStore.getState().refresh();
  },
});

let startDiscovery: () => void = () => client.retryNow();

if (browser) {
  if (MODE.mode === 'platform') {
    // No socket before the engine was seen / the user asked (spec §11 Browser). Discovery only sees the direct
    // socket: a relay connection must not count as «engine seen on this PC».
    const discovery = createDiscovery({
      client: {
        connect: () => client.connect(),
        retryNow: () => client.retryNow(),
        onStatus: (h) => client.onStatus((s, a) => (remote?.isDirect() ?? true) && h(s, a)),
      },
      storage: safeLocalStorage,
      // dev aid: `?lna=ignore` skips the permission query (embedded test browsers report `denied` for everything)
      permissions:
        MODE.dev && new URLSearchParams(location.search).get('lna') === 'ignore'
          ? undefined
          : (navigator.permissions as unknown as PermissionsLike | undefined),
      onState: (d) => cloudStore.getState().setDiscovery(d),
    });
    startDiscovery = () => discovery.start();
    import.meta.hot?.dispose(() => discovery.dispose());

    // Signed in, no local engine in this browser → the relay (docs/specs/2026-10-12-remote.md §6, §10).
    const q = new URLSearchParams(location.search).get('remote');
    const force: Route | null = MODE.dev && q === '1' ? 'relay' : MODE.dev && q === '0' ? 'direct' : null;
    const relay = new RelayLink({
      url: relayUrlFor(location, API_BASE, MODE.dev ? (import.meta.env.VITE_KS_RELAY as string | undefined) : undefined),
      refresh: getMe,
      storage: safeLocalStorage,
    });
    remote = createRemote({ client, cloud: cloudStore, relay, mode: MODE.mode, direct: directSocket, force, document });
    import.meta.hot?.dispose(() => relay.stop());
  } else {
    // Local UI (engine at :7340 or the dev server): connect right away, as before.
    cloudStore.getState().setDiscovery({ phase: 'connecting', seen: true, started: true, permission: 'unknown', failures: 0 });
    client.onStatus((s) =>
      cloudStore.getState().setDiscovery({
        phase: s === 'open' ? 'connected' : 'connecting',
        seen: true,
        started: true,
        permission: 'unknown',
        failures: 0,
      }),
    );
    client.connect();
  }
  // Update check once per page load, after the first contact with the engine.
  const offLatest = client.onStatus((s) => {
    if (s !== 'open') return;
    offLatest();
    void fetchLatestRelease().then((l) => cloudStore.getState().setLatest(l));
  });
}
// Recently loaded presets (Пульт), whoever loaded them.
engineStore.subscribe((s, prev) => {
  if (s.presetPath && s.presetPath !== prev.presetPath) noteRecent(s.presetPath);
});

// Vite HMR: a re-executed module creates a fresh client; drop the old socket.
import.meta.hot?.dispose(() => client.close());

/** «Подключить движок» / «Повторить» */
export const connectEngine = () => startDiscovery();
/** Drop the current socket (a non-keysynth server answered) and connect again. */
export const reconnectEngine = () => {
  client.close();
  client.connect();
};

export function useEngine<T>(selector: (s: EngineState) => T): T {
  return useStore(engineStore, selector);
}

export function useCloud<T>(selector: (s: CloudState) => T): T {
  return useStore(cloudStore, selector);
}

const directOnly = createStore<RemoteState>()(() => ({ route: 'direct', relay: IDLE_RELAY }));
/** route + relay state (always `direct` outside platform mode) */
export const remoteStore = () => remote?.store ?? directOnly;
export function useRemote<T>(selector: (s: RemoteState) => T): T {
  return useStore(remoteStore(), selector);
}
export const selectHost = (deviceId: string) => remote?.selectHost(deviceId);
/** «Повторить» on the relay route */
export const retryRelay = () => remote?.retry();

export function useSongs<T>(selector: (s: SongsState) => T): T {
  return useStore(songStore, selector);
}

export const actions = () => engineStore.getState();
export const songActions = () => songStore.getState();
export const cloudActions = () => cloudStore.getState();
