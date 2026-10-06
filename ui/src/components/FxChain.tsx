import { useEffect, useMemo, useRef, useState, type PointerEvent } from 'react';
import { actions, useEngine } from '../store';
import type { FxSlot, ModuleInfo, NodeId } from '../protocol/types';
import { insertionIndex, moveTarget } from '../lib/reorder';

function AddFxMenu({ layer, onClose }: { layer: NodeId; onClose: () => void }) {
  const catalog = useEngine((s) => s.catalog);
  const ref = useRef<HTMLDivElement>(null);
  const groups = useMemo(() => {
    const m = new Map<string, ModuleInfo[]>();
    for (const mod of catalog) {
      if (mod.kind !== 'effect') continue;
      const list = m.get(mod.category) ?? [];
      list.push(mod);
      m.set(mod.category, list);
    }
    return [...m.entries()].sort((a, b) => a[0].localeCompare(b[0]));
  }, [catalog]);
  useEffect(() => {
    const onDoc = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) onClose();
    };
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    document.addEventListener('mousedown', onDoc);
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('mousedown', onDoc);
      document.removeEventListener('keydown', onKey);
    };
  }, [onClose]);
  return (
    <div className="menu" ref={ref} role="menu">
      {groups.map(([cat, mods]) => (
        <div key={cat} className="menu-group">
          <div className="menu-head">{cat}</div>
          {mods.map((m) => (
            <button
              type="button"
              role="menuitem"
              key={m.typeId}
              className="menu-item"
              onClick={() => {
                void actions().addFx(layer, m.typeId);
                onClose();
              }}
            >
              {m.name}
            </button>
          ))}
        </div>
      ))}
      {groups.length === 0 && <div className="menu-head">Catalog not loaded</div>}
    </div>
  );
}

interface Props {
  layer: NodeId;
  fx: FxSlot[];
  vertical?: boolean;
  /** layer used for selection (master chain: undefined) */
  selectLayer?: NodeId;
}

/** FX chain: select, bypass, remove, drag to reorder, add from catalog by category. */
export function FxChain({ layer, fx, vertical, selectLayer }: Props) {
  const selected = useEngine((s) => s.selectedModule);
  const catalogMap = useEngine((s) => s.catalogMap);
  const [menu, setMenu] = useState(false);
  const [dragNode, setDragNode] = useState<NodeId | null>(null);
  const [dropAt, setDropAt] = useState<number | null>(null);
  const rowRef = useRef<HTMLDivElement>(null);
  const press = useRef<{ node: NodeId; x: number; y: number; moved: boolean } | null>(null);

  /** Insertion index (0..fx.length) for a pointer position, from the slot rects. */
  const indexAt = (x: number, y: number): number => {
    const slots = [...(rowRef.current?.querySelectorAll<HTMLElement>(':scope > .fx') ?? [])];
    return insertionIndex(slots.map((el) => el.getBoundingClientRect()), x, y, !!vertical);
  };
  const onPointerDown = (e: PointerEvent, node: NodeId | undefined) => {
    if (e.button !== 0 || node === undefined || (e.target as HTMLElement).closest('button')) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    press.current = { node, x: e.clientX, y: e.clientY, moved: false };
  };
  const onPointerMove = (e: PointerEvent) => {
    const p = press.current;
    if (!p) return;
    if (!p.moved && Math.hypot(e.clientX - p.x, e.clientY - p.y) < 5) return;
    p.moved = true;
    setDragNode(p.node);
    setDropAt(indexAt(e.clientX, e.clientY));
  };
  const onPointerUp = (e: PointerEvent) => {
    const p = press.current;
    press.current = null;
    if (!p) return;
    if (!p.moved) {
      actions().selectModule(p.node, selectLayer);
    } else {
      const to = moveTarget(
        fx.findIndex((f) => f.node === p.node),
        indexAt(e.clientX, e.clientY),
      );
      if (to !== null) void actions().moveFx(p.node, to);
    }
    setDragNode(null);
    setDropAt(null);
  };

  return (
    <div className={`fxchain ${vertical ? 'vertical' : ''} ${dragNode !== null ? 'is-dragging' : ''}`} ref={rowRef}>
      {fx.map((f, i) => {
        const info = catalogMap[f.type];
        const sel = f.node === selected;
        return (
          <div
            key={f.node ?? i}
            className={`fx ${sel ? 'on' : ''} ${f.bypass ? 'bypassed' : ''} ${dragNode === f.node ? 'dragging' : ''} ${
              dropAt === i ? 'drop-before' : ''
            } ${dropAt === i + 1 && i === fx.length - 1 ? 'drop-after' : ''}`}
            onPointerDown={(e) => onPointerDown(e, f.node)}
            onPointerMove={onPointerMove}
            onPointerUp={onPointerUp}
            onLostPointerCapture={() => {
              press.current = null;
              setDragNode(null);
              setDropAt(null);
            }}
            onPointerCancel={() => {
              press.current = null;
              setDragNode(null);
              setDropAt(null);
            }}
            title={`${info?.name ?? f.type} — drag to reorder`}
          >
            <span className="fx-grip" aria-hidden>
              ⋮⋮
            </span>
            <button
              type="button"
              className={`fx-power ${f.bypass ? '' : 'on'}`}
              aria-label={f.bypass ? 'Enable' : 'Bypass'}
              title={f.bypass ? 'Bypassed — click to enable' : 'Active — click to bypass'}
              onClick={(e) => {
                e.stopPropagation();
                if (f.node !== undefined) actions().setBypass(f.node, !f.bypass);
              }}
            />
            <span className="fx-name">{info?.name ?? f.type}</span>
            <button
              type="button"
              className="fx-x"
              aria-label="Remove effect"
              onClick={(e) => {
                e.stopPropagation();
                if (f.node !== undefined) void actions().removeFx(f.node);
              }}
            >
              ×
            </button>
          </div>
        );
      })}
      <div className="fx-add-wrap">
        <button type="button" className="fx-add" onClick={() => setMenu((v) => !v)} aria-haspopup="menu" aria-expanded={menu}>
          + FX
        </button>
        {menu && <AddFxMenu layer={layer} onClose={() => setMenu(false)} />}
      </div>
    </div>
  );
}
