import { useEffect, useRef, type PointerEvent } from 'react';
import type { ParamSpec } from '../protocol/types';
import { formatValue, fromNorm, isReadOnly, normScale, toNorm } from '../lib/paramMath';
import { live, onFrame } from '../store/liveBus';
import { Knob } from './Knob';

interface CtlProps {
  spec: ParamSpec;
  value: number;
  onChange: (v: number) => void;
}

export function Toggle({ spec, value, onChange, label }: CtlProps & { label?: string }) {
  const on = value >= 0.5;
  return (
    <div className="ctl ctl-toggle">
      <button
        type="button"
        className={`led-btn ${on ? 'on' : ''}`}
        aria-pressed={on}
        onClick={() => onChange(on ? 0 : 1)}
        onDoubleClick={(e) => e.preventDefault()}
      >
        <span className="led" />
        {label ?? spec.name}
      </button>
    </div>
  );
}

export function EnumControl({ spec, value, onChange }: CtlProps) {
  const idx = Math.round(value);
  const totalLen = spec.choices.reduce((n, c) => n + c.length, 0);
  if (spec.choices.length <= 4 && totalLen <= 18) {
    return (
      <div className="ctl ctl-enum">
        <div className="ctl-label">{spec.name}</div>
        <div className="segmented" role="radiogroup" aria-label={spec.name}>
          {spec.choices.map((c, i) => (
            <button
              type="button"
              key={c + i}
              role="radio"
              aria-checked={i === idx}
              className={i === idx ? 'on' : ''}
              onClick={() => onChange(i)}
            >
              {c}
            </button>
          ))}
        </div>
      </div>
    );
  }
  return (
    <div className="ctl ctl-enum">
      <div className="ctl-label">{spec.name}</div>
      <select className="select" value={idx} aria-label={spec.name} onChange={(e) => onChange(Number(e.target.value))}>
        {spec.choices.map((c, i) => (
          <option key={c + i} value={i}>
            {c}
          </option>
        ))}
      </select>
    </div>
  );
}

function drawbarColor(name: string): 'brown' | 'black' | 'white' {
  if (name.startsWith('16') || name.startsWith('5')) return 'brown';
  if (name.startsWith('2⅔') || name.startsWith('1⅗') || name.startsWith('1⅓')) return 'black';
  return 'white';
}

/** Hammond-style drawbar: pull down to increase. */
export function Drawbar({ spec, value, onChange }: CtlProps) {
  const drag = useRef<{ y: number; norm: number } | null>(null);
  const norm = toNorm(spec, value);
  const travel = 96;
  const slotRef = useRef<HTMLDivElement>(null);
  const latest = useRef({ spec, value, onChange });
  latest.current = { spec, value, onChange };
  // non-passive wheel listener so the page does not scroll while adjusting
  useEffect(() => {
    const el = slotRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const { spec: s, value: v, onChange: cb } = latest.current;
      const next = Math.max(s.min, Math.min(s.max, Math.round(v + (e.deltaY > 0 ? 1 : -1))));
      if (next !== v) cb(next);
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, []);
  const endDrag = () => {
    drag.current = null;
  };
  const onDown = (e: PointerEvent) => {
    if (e.button !== 0) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    drag.current = { y: e.clientY, norm };
  };
  const onMove = (e: PointerEvent) => {
    const d = drag.current;
    if (!d) return;
    d.norm = Math.min(1, Math.max(0, d.norm + (e.clientY - d.y) / travel));
    d.y = e.clientY;
    const v = fromNorm(spec, d.norm);
    if (v !== value) onChange(v);
  };
  return (
    <div className="drawbar">
      <div
        className="drawbar-slot"
        role="slider"
        tabIndex={0}
        aria-label={spec.name}
        aria-valuemin={spec.min}
        aria-valuemax={spec.max}
        aria-valuenow={value}
        onPointerDown={onDown}
        onPointerMove={onMove}
        ref={slotRef}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onLostPointerCapture={endDrag}
        onDoubleClick={() => onChange(spec.def)}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown') {
            e.preventDefault();
            onChange(Math.min(spec.max, value + 1));
          }
          if (e.key === 'ArrowUp') {
            e.preventDefault();
            onChange(Math.max(spec.min, value - 1));
          }
        }}
      >
        <div className={`drawbar-rod ${drawbarColor(spec.name)}`} style={{ height: 14 + norm * travel }}>
          <div className="drawbar-ticks">
            {Array.from({ length: Math.round(value) }, (_, i) => (
              <span key={i}>{i + 1}</span>
            ))}
          </div>
          <div className="drawbar-cap">{spec.name}</div>
        </div>
      </div>
      <div className="knob-readout">{Math.round(value)}</div>
    </div>
  );
}

/** ReadOnly param display, driven from telemetry readouts at frame rate (no React re-render). */
export function Readout({ spec, node }: { spec: ParamSpec; node: number }) {
  const barRef = useRef<HTMLDivElement>(null);
  const txtRef = useRef<HTMLSpanElement>(null);
  const isGr = spec.unit === 'dB' && spec.max <= 0;
  useEffect(() => {
    let shown = Number.NaN;
    let smooth = spec.def;
    return onFrame(() => {
      const v = live.telemetry?.readouts?.[String(node)]?.[spec.id];
      if (v === undefined) return;
      smooth += (v - smooth) * 0.35;
      const n = toNorm(spec, smooth);
      if (barRef.current) {
        barRef.current.style.transform = isGr ? `scaleX(${1 - n})` : `scaleX(${n})`;
      }
      const q = normScale(spec.scale) === 'linear' || normScale(spec.scale) === 'log' ? Math.round(smooth * 10) / 10 : Math.round(smooth);
      if (txtRef.current && q !== shown) {
        shown = q;
        txtRef.current.textContent = formatValue(spec, q);
      }
    });
  }, [spec, node, isGr]);
  return (
    <div className={`ctl ctl-readout ${isGr ? 'gr' : ''}`}>
      <div className="ctl-label">{spec.name}</div>
      <div className="readout-bar">
        <div ref={barRef} className="readout-fill" />
      </div>
      <span ref={txtRef} className="readout-text">
        {formatValue(spec, spec.def)}
      </span>
    </div>
  );
}

/** Pick the control for a ParamSpec. */
export function ParamControl({
  spec,
  value,
  onChange,
  node,
  style,
}: CtlProps & { node: number; style?: 'knob' | 'slider' | 'drawbar' }) {
  if (isReadOnly(spec)) return <Readout spec={spec} node={node} />;
  if (style === 'drawbar') return <Drawbar spec={spec} value={value} onChange={onChange} />;
  switch (normScale(spec.scale)) {
    case 'bool':
      return <Toggle spec={spec} value={value} onChange={onChange} />;
    case 'enum':
      return <EnumControl spec={spec} value={value} onChange={onChange} />;
    default:
      return <Knob spec={spec} value={value} onChange={onChange} />;
  }
}
