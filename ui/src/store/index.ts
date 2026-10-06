import { useStore } from 'zustand';
import { EngineClient, engineUrlFromLocation } from '../protocol/client';
import { bindClient, createEngineStore, type EngineState } from './engineStore';

const url = typeof location !== 'undefined' ? engineUrlFromLocation(location.search) : undefined;

export const client = new EngineClient(url ? { url } : {});
export const engineStore = createEngineStore({ client });
bindClient(engineStore, client);
if (typeof window !== 'undefined') client.connect();
// Vite HMR: a re-executed module creates a fresh client; drop the old socket.
import.meta.hot?.dispose(() => client.close());

export function useEngine<T>(selector: (s: EngineState) => T): T {
  return useStore(engineStore, selector);
}

export const actions = () => engineStore.getState();
