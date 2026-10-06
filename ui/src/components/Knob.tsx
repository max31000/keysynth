import { useCallback, useEffect, useRef, useState, type KeyboardEvent, type PointerEvent } from 'react';
import type { ParamSpec } from '../protocol/types';
import { dragNorm, formatValue, fromNorm, normScale, toNorm, wheelValue } from '../lib/paramMath';

interface Props {
  spec: ParamSpec;
  value: number;
  onChange: (v: number) => void;
  size?: number;
  label?: string;
  /** hide label/value text (compact strips) */
  bare?: boolean;
  className?: string;
}

const START = -135;
const SWEEP = 270;

function polar(cx: number, cy: number, r: number, deg: number) {
  const a = ((deg - 90) * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)] as const;
}

function arc(cx: number, cy: number, r: number, a0: number, a1: number) {
  if (Math.abs(a1 - a0) < 0.01) return '';
  const [x0, y0] = polar(cx, cy, r, Math.min(a0, a1));
  const [x1, y1] = polar(cx, cy, r, Math.max(a0, a1));
  const large = Math.abs(a1 - a0) > 180 ? 1 : 0;
  return `M ${x0} ${y0} A ${r} ${r} 0 ${large} 1 ${x1} ${y1}`;
}

/** Rotary control. Drag vertically (shift = fine), wheel, double-click resets to default, arrows step. */
export function Knob({ spec, value, onChange, size = 44, label, bare, className }: Props) {
  const ref = useRef<HTMLDivElement>(null);
  const drag = useRef<{ y: number; norm: number } | null>(null);
  const [active, setActive] = useState(false);
  const norm = toNorm(spec, value);
  const bipolar = normScale(spec.scale) === 'linear' && spec.min < 0 && spec.max > 0;
  const originNorm = bipolar ? toNorm(spec, 0) : 0;
  const angle = START + SWEEP * norm;
  const origin = START + SWEEP * originNorm;
  const r = size / 2;
  const cx = r;
  const cy = r;
  const ringR = r - 3;
  const bodyR = r - 8;
  const [ix0, iy0] = polar(cx, cy, bodyR * 0.25, angle);
  const [ix1, iy1] = polar(cx, cy, bodyR - 2, angle);
  const latest = useRef({ spec, value, onChange });
  latest.current = { spec, value, onChange };

  const emit = useCallback((v: number) => {
    const { value: cur, onChange: cb } = latest.current;
    if (v !== cur) cb(v);
  }, []);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const { spec: s, value: v } = latest.current;
      const notches = -Math.sign(e.deltaY);
      if (notches) emit(wheelValue(s, v, notches, e.shiftKey));
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [emit]);

  const onPointerDown = (e: PointerEvent) => {
    if (e.button !== 0) return;
    e.preventDefault();
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    (e.currentTarget as HTMLElement).focus();
    drag.current = { y: e.clientY, norm };
    setActive(true);
  };
  const onPointerMove = (e: PointerEvent) => {
    const d = drag.current;
    if (!d) return;
    const dy = e.clientY - d.y;
    d.y = e.clientY;
    d.norm = dragNorm(d.norm, dy, e.shiftKey);
    emit(fromNorm(latest.current.spec, d.norm));
  };
  const onPointerUp = () => {
    drag.current = null;
    setActive(false);
  };
  const onKeyDown = (e: KeyboardEvent) => {
    const dir = e.key === 'ArrowUp' || e.key === 'ArrowRight' ? 1 : e.key === 'ArrowDown' || e.key === 'ArrowLeft' ? -1 : 0;
    if (dir) {
      e.preventDefault();
      emit(wheelValue(spec, value, dir * (e.shiftKey ? 0.1 : 1), false));
    } else if (e.key === 'Home' || e.key === 'Delete') {
      e.preventDefault();
      emit(fromNorm(spec, toNorm(spec, spec.def)));
    }
  };

  const text = formatValue(spec, value);
  const name = label ?? spec.name;

  return (
    <div className={`knob ${active ? 'is-active' : ''} ${className ?? ''}`} style={{ width: Math.max(size, 58) }}>
      <div
        ref={ref}
        className="knob-hit"
        role="slider"
        tabIndex={0}
        aria-label={name}
        aria-valuemin={spec.min}
        aria-valuemax={spec.max}
        aria-valuenow={value}
        aria-valuetext={text}
        title={`${name}: ${text}\nDrag (Shift = fine) · wheel · double-click = default`}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerCancel={onPointerUp}
        onLostPointerCapture={onPointerUp}
        onDoubleClick={() => emit(fromNorm(spec, toNorm(spec, spec.def)))}
        onKeyDown={onKeyDown}
        style={{ width: size, height: size }}
      >
        <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} aria-hidden>
          <path className="knob-track" d={arc(cx, cy, ringR, START, START + SWEEP)} />
          <path className="knob-value" d={arc(cx, cy, ringR, origin, angle)} />
          <circle className="knob-body" cx={cx} cy={cy} r={bodyR} />
          <circle className="knob-cap" cx={cx} cy={cy} r={bodyR - 3} />
          <line className="knob-pointer" x1={ix0} y1={iy0} x2={ix1} y2={iy1} />
        </svg>
        {active && <div className="knob-tip">{text}</div>}
      </div>
      {!bare && (
        <>
          <div className="knob-label">{name}</div>
          <div className="knob-readout">{text}</div>
        </>
      )}
    </div>
  );
}
