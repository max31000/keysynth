// Pure, immutable patch helpers used by the store's optimistic updates.

import { MASTER_NODE, type FxSlot, type Layer, type ModuleSlot, type NodeId, type Patch, type Zone } from '../protocol/types';

export interface ModuleLocation {
  slot: ModuleSlot;
  /** owning layer (undefined = master chain or rhythm) */
  layer?: Layer;
  /** `rhythm` = the drum-sequencer kit (patch.rhythm) */
  kind: 'instrument' | 'fx' | 'rhythm';
}

/** The RhythmNode's kit as a module slot (type `drums`, params = `rhythm.kit`). */
export function rhythmSlot(patch: Patch | null): ModuleSlot | null {
  const r = patch?.rhythm;
  if (!r || r.node === undefined) return null;
  return { node: r.node, type: 'drums', params: r.kit ?? {} };
}

export function findModule(patch: Patch | null, node: NodeId): ModuleLocation | null {
  if (!patch) return null;
  for (const layer of patch.layers) {
    if (layer.instrument.node === node) return { slot: layer.instrument, layer, kind: 'instrument' };
    const fx = layer.fx.find((f) => f.node === node);
    if (fx) return { slot: fx, layer, kind: 'fx' };
  }
  const mfx = patch.master.fx.find((f) => f.node === node);
  if (mfx) return { slot: mfx, kind: 'fx' };
  const rs = rhythmSlot(patch);
  return rs && rs.node === node ? { slot: rs, kind: 'rhythm' } : null;
}

export function findLayer(patch: Patch | null, node: NodeId): Layer | null {
  return patch?.layers.find((l) => l.node === node) ?? null;
}

const mapFx = (fx: FxSlot[], node: NodeId, f: (s: FxSlot) => FxSlot) =>
  fx.some((s) => s.node === node) ? fx.map((s) => (s.node === node ? f(s) : s)) : fx;

function mapSlot(patch: Patch, node: NodeId, f: <S extends ModuleSlot>(s: S) => S): Patch {
  let changed = false;
  const layers = patch.layers.map((l) => {
    if (l.instrument.node === node) {
      changed = true;
      return { ...l, instrument: f(l.instrument) };
    }
    const fx = mapFx(l.fx, node, f);
    if (fx !== l.fx) {
      changed = true;
      return { ...l, fx };
    }
    return l;
  });
  const mfx = mapFx(patch.master.fx, node, f);
  if (mfx !== patch.master.fx) changed = true;
  let rhythm = patch.rhythm;
  const rs = !changed ? rhythmSlot(patch) : null;
  if (rs && rs.node === node) {
    const ns = f(rs);
    if (ns !== rs) {
      rhythm = { ...patch.rhythm, kit: ns.params };
      changed = true;
    }
  }
  if (!changed) return patch;
  return {
    ...patch,
    layers,
    master: mfx === patch.master.fx ? patch.master : { ...patch.master, fx: mfx },
    ...(rhythm ? { rhythm } : {}),
  };
}

/** Set a param value. Node 0 + `volume_db` addresses the master volume. Unknown node → unchanged. */
export function applyParam(patch: Patch, node: NodeId, param: string, value: number): Patch {
  if (node === MASTER_NODE) {
    if (param === 'volume_db') return { ...patch, master: { ...patch.master, volume_db: value } };
    return patch;
  }
  return mapSlot(patch, node, (s) => (s.params[param] === value ? s : { ...s, params: { ...s.params, [param]: value } }));
}

export function applyZone(patch: Patch, layer: NodeId, zone: Partial<Zone>): Patch {
  if (!patch.layers.some((l) => l.node === layer)) return patch;
  return {
    ...patch,
    layers: patch.layers.map((l) => (l.node === layer ? { ...l, zone: { ...l.zone, ...zone } } : l)),
  };
}

export function applyBypass(patch: Patch, node: NodeId, bypass: boolean): Patch {
  return mapSlot(patch, node, (s) => ({ ...s, bypass }));
}

/** Reorder an FX within its chain (optimistic preview for move_fx). */
export function applyMoveFx(patch: Patch, node: NodeId, to: number): Patch {
  const move = (fx: FxSlot[]) => {
    const from = fx.findIndex((f) => f.node === node);
    if (from < 0) return fx;
    const next = fx.slice();
    const [item] = next.splice(from, 1);
    next.splice(Math.max(0, Math.min(to, next.length)), 0, item!);
    return next;
  };
  const layers = patch.layers.map((l) => {
    const fx = move(l.fx);
    return fx === l.fx ? l : { ...l, fx };
  });
  const mfx = move(patch.master.fx);
  return { ...patch, layers, master: mfx === patch.master.fx ? patch.master : { ...patch.master, fx: mfx } };
}

/** Solo logic: a layer is audible if not muted and (no solo anywhere or it is soloed). */
export function isAudible(patch: Patch, layer: Layer): boolean {
  const anySolo = patch.layers.some((l) => l.zone.solo);
  return !layer.zone.mute && (!anySolo || layer.zone.solo);
}
