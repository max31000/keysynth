// Generic module-panel layout from ParamSpec groups + uiHints (groups, tabs). Rendered by components/ModulePanel.
import { isHidden } from './paramMath';
import type { ModuleInfo, ParamSpec } from '../protocol/types';

export interface ParamGroup {
  name: string;
  params: ParamSpec[];
}

export interface PanelTab {
  name: string;
  groups: ParamGroup[];
}

/** Modules with more visible params than this get a tabbed panel even without `uiHints.tabs`. */
export const AUTO_TAB_THRESHOLD = 40;
export const FRONT_TAB = 'Front';

/** Groups in display order: uiHints.groupOrder first, then first-appearance order. */
export function groupParams(info: ModuleInfo): ParamGroup[] {
  const map = new Map<string, ParamSpec[]>();
  for (const p of info.params) {
    if (isHidden(p)) continue;
    const g = p.group || 'Main';
    const list = map.get(g) ?? [];
    list.push(p);
    map.set(g, list);
  }
  const order = info.uiHints?.groupOrder ?? [];
  const names = [...order.filter((g) => map.has(g)), ...[...map.keys()].filter((g) => !order.includes(g))];
  return names.map((name) => ({ name, params: map.get(name)! }));
}

/**
 * Tab layout for large modules: `uiHints.tabs` bundles param groups into tabs; groups not listed get a tab of their
 * own, in group order. Without `tabs`, modules above AUTO_TAB_THRESHOLD visible params get one tab per group. A
 * `Front` tab with the `uiHints.front` params comes first when present. Returns null for a flat (untabbed) panel.
 */
export function panelTabs(info: ModuleInfo, groups: ParamGroup[] = groupParams(info)): PanelTab[] | null {
  const hintTabs = info.uiHints?.tabs;
  const visible = groups.reduce((n, g) => n + g.params.length, 0);
  if (!hintTabs?.length && visible <= AUTO_TAB_THRESHOLD) return null;
  const byName = new Map(groups.map((g) => [g.name, g]));
  const used = new Set<string>();
  const tabs: PanelTab[] = [];
  for (const t of hintTabs ?? []) {
    const gs = t.groups.map((n) => byName.get(n)).filter((g): g is ParamGroup => !!g && !used.has(g.name));
    if (!gs.length) continue;
    gs.forEach((g) => used.add(g.name));
    tabs.push({ name: t.name, groups: gs });
  }
  for (const g of groups) if (!used.has(g.name)) tabs.push({ name: g.name, groups: [g] });
  const ids = new Map(info.params.map((p) => [p.id, p]));
  const front = (info.uiHints?.front ?? []).map((id) => ids.get(id)).filter((p): p is ParamSpec => !!p && !isHidden(p));
  if (front.length) tabs.unshift({ name: FRONT_TAB, groups: [{ name: FRONT_TAB, params: front }] });
  return tabs.length > 1 ? tabs : null;
}
