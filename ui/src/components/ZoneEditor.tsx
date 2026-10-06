import { useMemo, useRef, type PointerEvent } from 'react';
import { keyLayout, noteName, PIANO_HI, PIANO_LO } from '../lib/notes';

interface Props {
  lo: number;
  hi: number;
  onChange: (lo: number, hi: number) => void;
  dimmed?: boolean;
}

const H = 22;

/** Key range drawn on a mini 88-key keyboard; drag the edges (or click to move the nearest edge). */
export function ZoneEditor({ lo, hi, onChange, dimmed }: Props) {
  const { keys, whiteCount } = useMemo(() => keyLayout(PIANO_LO, PIANO_HI), []);
  const svgRef = useRef<SVGSVGElement>(null);
  const dragging = useRef<'lo' | 'hi' | null>(null);
  const xOf = (note: number) => {
    const k = keys.find((g) => g.note === note);
    return k ? k.x : 0;
  };
  const rightOf = (note: number) => {
    const k = keys.find((g) => g.note === note);
    return k ? k.x + k.w : whiteCount;
  };
  const noteAt = (clientX: number): number => {
    const el = svgRef.current;
    if (!el) return lo;
    const r = el.getBoundingClientRect();
    const x = ((clientX - r.left) / r.width) * whiteCount;
    // prefer black keys (upper half) only by x; fine for a mini editor
    let best = PIANO_LO;
    let bestD = Infinity;
    for (const k of keys) {
      const c = k.x + k.w / 2;
      const d = Math.abs(c - x) * (k.black ? 1.25 : 1);
      if (d < bestD) {
        bestD = d;
        best = k.note;
      }
    }
    return best;
  };
  const apply = (which: 'lo' | 'hi', n: number) => {
    if (which === 'lo') onChange(Math.min(n, hi), hi);
    else onChange(lo, Math.max(n, lo));
  };
  const onDown = (e: PointerEvent<SVGSVGElement>) => {
    if (e.button !== 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    const n = noteAt(e.clientX);
    const which = Math.abs(n - lo) <= Math.abs(n - hi) ? 'lo' : 'hi';
    dragging.current = which;
    apply(which, n);
  };
  const onMove = (e: PointerEvent<SVGSVGElement>) => {
    if (dragging.current) apply(dragging.current, noteAt(e.clientX));
  };

  const x0 = xOf(lo);
  const x1 = rightOf(hi);
  return (
    <div className={`zone ${dimmed ? 'dimmed' : ''}`}>
      <svg
        ref={svgRef}
        className="zone-kbd"
        viewBox={`0 0 ${whiteCount} ${H}`}
        preserveAspectRatio="none"
        onPointerDown={onDown}
        onPointerMove={onMove}
        onPointerUp={() => (dragging.current = null)}
        onPointerCancel={() => (dragging.current = null)}
        onLostPointerCapture={() => (dragging.current = null)}
        onDoubleClick={() => onChange(PIANO_LO, PIANO_HI)}
        role="group"
        aria-label={`Key range ${noteName(lo)} to ${noteName(hi)}`}
      >
        {keys
          .filter((k) => !k.black)
          .map((k) => (
            <rect
              key={k.note}
              x={k.x + 0.04}
              y={0}
              width={k.w - 0.08}
              height={H}
              className={`zk-w ${k.note >= lo && k.note <= hi ? 'in' : ''}`}
            />
          ))}
        {keys
          .filter((k) => k.black)
          .map((k) => (
            <rect key={k.note} x={k.x} y={0} width={k.w} height={H * 0.6} className={`zk-b ${k.note >= lo && k.note <= hi ? 'in' : ''}`} />
          ))}
        <rect className="zone-range" x={x0} y={0} width={Math.max(0.2, x1 - x0)} height={H} />
        <rect className="zone-handle" x={x0} y={0} width={0.35} height={H} />
        <rect className="zone-handle" x={x1 - 0.35} y={0} width={0.35} height={H} />
      </svg>
      <div className="zone-text mono">
        {noteName(lo)}–{noteName(hi)}
      </div>
    </div>
  );
}
