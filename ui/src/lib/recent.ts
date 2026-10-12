// Recently loaded presets (this browser), newest first; persisted in localStorage. Fed from the engine's
// `presetPath` (store/index.ts), so a preset loaded from another tab or the PC counts too.

import { useSyncExternalStore } from 'react';

const KEY = 'keysynth.recentPresets';
export const RECENT_MAX = 12;

/** Pure: `path` moved to the front, deduplicated, capped. */
export function pushRecent(list: readonly string[], path: string, max = RECENT_MAX): string[] {
  return [path, ...list.filter((p) => p !== path)].slice(0, max);
}

const listeners = new Set<() => void>();
let cache: string[] = (() => {
  try {
    const raw = typeof localStorage !== 'undefined' ? localStorage.getItem(KEY) : null;
    const arr: unknown = raw ? JSON.parse(raw) : [];
    return Array.isArray(arr) ? arr.filter((x): x is string => typeof x === 'string').slice(0, RECENT_MAX) : [];
  } catch {
    return [];
  }
})();

export function noteRecent(path: string): void {
  if (cache[0] === path) return;
  cache = pushRecent(cache, path);
  try {
    localStorage.setItem(KEY, JSON.stringify(cache));
  } catch {
    /* storage unavailable: keep in memory */
  }
  for (const l of listeners) l();
}

export function useRecent(): string[] {
  return useSyncExternalStore(
    (cb) => {
      listeners.add(cb);
      return () => listeners.delete(cb);
    },
    () => cache,
  );
}
