// Editor or «Пульт» (touch remote view, docs/specs/2026-10-12-remote.md §6). Remembered per browser; the first
// visit on a touch tablet (coarse pointer, ≥ 768 px) opens the Пульт.

import { useCallback, useState } from 'react';

export type AppView = 'editor' | 'pult';
const KEY = 'keysynth.view';

export function defaultView(i: { stored: string | null; coarse: boolean; width: number }): AppView {
  if (i.stored === 'editor' || i.stored === 'pult') return i.stored;
  return i.coarse && i.width >= 768 ? 'pult' : 'editor';
}

function initial(): AppView {
  let stored: string | null = null;
  try {
    stored = localStorage.getItem(KEY);
  } catch {
    /* private mode */
  }
  const coarse = typeof matchMedia !== 'undefined' && matchMedia('(pointer: coarse)').matches;
  return defaultView({ stored, coarse, width: typeof innerWidth === 'number' ? innerWidth : 0 });
}

export function useView(): [AppView, (v: AppView) => void] {
  const [view, setState] = useState<AppView>(initial);
  const set = useCallback((v: AppView) => {
    setState(v);
    try {
      localStorage.setItem(KEY, v);
    } catch {
      /* private mode */
    }
  }, []);
  return [view, set];
}
