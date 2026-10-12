import { useStore } from 'zustand';
import { EngineClient } from '../protocol/client';
import { bindClient, createEngineStore, type EngineState } from './engineStore';
import { createCloudStore, type CloudState } from './cloudStore';
import { MODE } from '../platform/mode';
import { createDiscovery, type PermissionsLike } from '../platform/discovery';
import { listDevices, mintDevice } from '../platform/api';
import { fetchLatestRelease } from '../platform/version';

const browser = typeof window !== 'undefined';
const safeLocalStorage = (() => {
  try {
    return browser ? window.localStorage : null;
  } catch {
    return null;
  }
})();

// The platform page probes less eagerly (LNA prompt): back off up to 10 s.
export const client = new EngineClient(MODE.mode === 'platform' ? { url: MODE.engineUrl, backoffMaxMs: 10000 } : { url: MODE.engineUrl });
export const engineStore = createEngineStore({ client });
bindClient(engineStore, client, { acceptMock: MODE.dev });

export const cloudStore = createCloudStore({
  client,
  mode: MODE.mode,
  acceptMock: MODE.dev,
  api: { mint: mintDevice, listDevices },
  storage: safeLocalStorage,
  onProfileChanged: () => {
    void engineStore.getState().refreshPresets();
    void engineStore.getState().refreshPatterns();
  },
});

let startDiscovery: () => void = () => client.retryNow();

if (browser) {
  if (MODE.mode === 'platform') {
    // No socket before the engine was seen / the user asked (spec §11 Browser).
    const discovery = createDiscovery({
      client,
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

export const actions = () => engineStore.getState();
export const cloudActions = () => cloudStore.getState();
