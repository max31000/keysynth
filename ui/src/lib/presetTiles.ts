// Preset tiles of the Пульт (components/remote/RemoteView.tsx).

import type { PresetEntry } from '../protocol/types';

export type PresetTab = 'fav' | 'recent' | 'all';

/** Presets shown for a tab, or every match of a non-empty query (name, category, tags). */
export function visiblePresets(presets: PresetEntry[], tab: PresetTab, query: string, favs: Set<string>, recent: string[]): PresetEntry[] {
  const q = query.trim().toLowerCase();
  if (q) {
    return presets.filter(
      (p) => p.name.toLowerCase().includes(q) || p.category.toLowerCase().includes(q) || p.tags.some((t) => t.toLowerCase().includes(q)),
    );
  }
  if (tab === 'fav') return presets.filter((p) => favs.has(p.path));
  if (tab === 'recent') {
    const byPath = new Map(presets.map((p) => [p.path, p]));
    return recent.map((r) => byPath.get(r)).filter((p): p is PresetEntry => !!p);
  }
  return presets;
}
