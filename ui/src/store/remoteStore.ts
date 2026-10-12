// Route between the direct engine socket and the relay (platform/route.ts) and the relay's state for the UI.
// One EngineClient serves both: its socket factory asks `createSocket`, and a route change swaps the transport
// (pending requests reject with `disconnected`; the new socket sends `hello` again).

import { createStore, type StoreApi } from 'zustand/vanilla';
import type { EngineClient, WebSocketFactory, WebSocketLike } from '../protocol/client';
import type { CloudState } from './cloudStore';
import type { RelayLink, RelayState } from '../platform/relay';
import { decideRoute, type Route } from '../platform/route';
import type { AppMode } from '../platform/mode';

export interface RemoteState {
  route: Route;
  relay: RelayState;
}

export interface DocumentLike {
  readonly visibilityState: string;
  addEventListener(type: 'visibilitychange', fn: () => void): void;
}

export interface RemoteDeps {
  client: Pick<EngineClient, 'switchTransport' | 'retryNow'>;
  cloud: StoreApi<CloudState>;
  /** null outside platform mode: the route is always direct */
  relay: RelayLink | null;
  mode: AppMode;
  direct: WebSocketFactory;
  force?: Route | null;
  document?: DocumentLike | null;
}

export interface Remote {
  store: StoreApi<RemoteState>;
  /** EngineClient's socket factory */
  createSocket(url: string): WebSocketLike;
  isDirect(): boolean;
  selectHost(deviceId: string): void;
  /** «Повторить» while the relay is down */
  retry(): void;
}

export const IDLE_RELAY: RelayState = { status: 'idle', hosts: [], hostId: null, rtt: null, error: null, failures: 0 };

export function createRemote(deps: RemoteDeps): Remote {
  const { client, cloud, relay } = deps;
  const store = createStore<RemoteState>()(() => ({ route: 'direct', relay: relay?.getState() ?? IDLE_RELAY }));
  relay?.subscribe(() => store.setState({ relay: relay.getState() }));

  const apply = () => {
    const s = cloud.getState();
    const current = store.getState().route;
    const route: Route = relay ? decideRoute({ mode: deps.mode, me: s.me, discovery: s.discovery, force: deps.force ?? null, current }) : 'direct';
    if (route === current) return;
    store.setState({ route });
    if (route === 'relay') {
      relay!.start();
      client.switchTransport(true);
    } else {
      // back to the local engine: connect only if discovery runs (no unasked Local Network Access prompt). The
      // client lets go of the relay transport first, so its close does not reach discovery as a failed attempt.
      client.switchTransport(s.discovery.started);
      relay?.stop();
    }
  };
  cloud.subscribe(apply);
  apply();

  const doc = deps.document;
  if (relay && doc) {
    relay.setVisible(doc.visibilityState === 'visible');
    doc.addEventListener('visibilitychange', () => {
      const visible = doc.visibilityState === 'visible';
      relay.setVisible(visible);
      if (visible && store.getState().route === 'relay') client.retryNow();
    });
  }

  return {
    store,
    createSocket: (url) => (relay && store.getState().route === 'relay' ? relay.transport() : deps.direct(url)),
    isDirect: () => store.getState().route === 'direct',
    selectHost: (id) => relay?.selectHost(id),
    retry: () => {
      relay?.retryNow();
      client.retryNow();
    },
  };
}
