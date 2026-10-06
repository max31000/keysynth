import { useCallback, useSyncExternalStore } from 'react';

const KEY = 'keysynth.favourites';
const listeners = new Set<() => void>();

function read(): Set<string> {
  try {
    const raw = localStorage.getItem(KEY);
    const arr: unknown = raw ? JSON.parse(raw) : [];
    return new Set(Array.isArray(arr) ? arr.filter((x): x is string => typeof x === 'string') : []);
  } catch {
    return new Set();
  }
}

let cache: Set<string> = typeof localStorage !== 'undefined' ? read() : new Set();

export function toggleFavourite(path: string): void {
  const next = new Set(cache);
  if (next.has(path)) next.delete(path);
  else next.add(path);
  cache = next;
  try {
    localStorage.setItem(KEY, JSON.stringify([...next]));
  } catch {
    /* storage unavailable: keep in memory */
  }
  for (const l of listeners) l();
}

/** Favourite preset paths, persisted in localStorage. */
export function useFavourites(): [Set<string>, (path: string) => void] {
  const favs = useSyncExternalStore(
    (cb) => {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    () => cache,
  );
  const toggle = useCallback((p: string) => toggleFavourite(p), []);
  return [favs, toggle];
}
