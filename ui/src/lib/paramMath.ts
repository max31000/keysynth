// Param value ⇄ normalised (0..1) control position, quantisation and display formatting.
// Semantics follow ARCHITECTURE §5.1 (ParamSpec).

import { ParamFlags, type ParamScale, type ParamSpec } from '../protocol/types';

export function normScale(s: string): ParamScale {
  const l = s.toLowerCase();
  return l === 'log' || l === 'int' || l === 'enum' || l === 'bool' ? l : 'linear';
}

export const isReadOnly = (p: ParamSpec) => (p.flags & ParamFlags.ReadOnly) !== 0;
export const isHidden = (p: ParamSpec) => (p.flags & ParamFlags.Hidden) !== 0;

const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);
export const clamp01 = (v: number) => clamp(v, 0, 1);

/** Exponent k such that norm = t^k maps the skew centre to 0.5 (JUCE-style skew). */
export function skewExponent(min: number, max: number, centre: number): number {
  const t = (centre - min) / (max - min);
  if (!(t > 0 && t < 1)) return 1;
  return Math.log(0.5) / Math.log(t);
}

/** Bounds actually used for Enum (index range), others use spec min/max. */
function range(p: ParamSpec): [number, number] {
  // Enum without labels (malformed spec): fall back to the declared index range
  if (normScale(p.scale) === 'enum') return p.choices.length ? [0, p.choices.length - 1] : [p.min, Math.max(p.min, p.max)];
  if (normScale(p.scale) === 'bool') return [0, 1];
  return [p.min, p.max];
}

/** Snap and clamp a plain value to what the param can hold. */
export function quantize(p: ParamSpec, v: number): number {
  const [lo, hi] = range(p);
  if (!Number.isFinite(v)) v = p.def;
  const c = clamp(v, lo, hi);
  switch (normScale(p.scale)) {
    case 'int':
    case 'enum':
      return Math.round(c);
    case 'bool':
      return c >= 0.5 ? 1 : 0;
    default:
      return c;
  }
}

/** Plain value → 0..1 control position. */
export function toNorm(p: ParamSpec, value: number): number {
  const [lo, hi] = range(p);
  if (hi <= lo) return 0;
  const v = clamp(value, lo, hi);
  const scale = normScale(p.scale);
  if (scale === 'log') {
    if (p.skewCentre > 0) {
      const t = (v - lo) / (hi - lo);
      return clamp01(Math.pow(t, skewExponent(lo, hi, p.skewCentre)));
    }
    if (lo > 0) return clamp01(Math.log(v / lo) / Math.log(hi / lo));
  }
  return (v - lo) / (hi - lo);
}

/** 0..1 control position → plain value (quantised). */
export function fromNorm(p: ParamSpec, norm: number): number {
  const [lo, hi] = range(p);
  const n = clamp01(norm);
  const scale = normScale(p.scale);
  let v: number;
  if (scale === 'log' && p.skewCentre > 0) {
    v = lo + (hi - lo) * Math.pow(n, 1 / skewExponent(lo, hi, p.skewCentre));
  } else if (scale === 'log' && lo > 0) {
    v = lo * Math.pow(hi / lo, n);
  } else {
    v = lo + (hi - lo) * n;
  }
  return quantize(p, v);
}

/** Number of discrete steps (Int/Enum/Bool), or 0 for continuous params. */
export function stepCount(p: ParamSpec): number {
  const s = normScale(p.scale);
  const [lo, hi] = range(p);
  return s === 'int' || s === 'enum' || s === 'bool' ? Math.round(hi - lo) : 0;
}

/** Pixel drag → new plain value. 200 px = full range; fine (shift) = 10× slower. */
export function dragValue(p: ParamSpec, startNorm: number, dyPixels: number, fine: boolean): number {
  return fromNorm(p, dragNorm(startNorm, dyPixels, fine));
}

/** Incremental drag on the normalised position (dy > 0 = pointer moved down). Kept unquantised so Int/Enum
 *  params step smoothly while dragging. */
export function dragNorm(norm: number, dyPixels: number, fine: boolean): number {
  return clamp01(norm - dyPixels / (fine ? 2000 : 200));
}

/** Wheel step: one notch = 1/100 range (1/1000 fine) for continuous, one step for discrete. */
export function wheelValue(p: ParamSpec, value: number, notches: number, fine: boolean): number {
  const steps = stepCount(p);
  if (steps > 0) return quantize(p, value + Math.sign(notches) * Math.max(1, Math.round(Math.abs(notches))));
  return fromNorm(p, toNorm(p, value) + notches * (fine ? 0.001 : 0.01));
}

function trimNum(v: number, digits: number): string {
  return v.toFixed(digits).replace(/\.?0+$/, '') || '0';
}

function smartDigits(abs: number): number {
  if (abs >= 100) return 0;
  if (abs >= 10) return 1;
  if (abs >= 1) return 2;
  return 3;
}

/** Human-readable value with unit. */
export function formatValue(p: ParamSpec, value: number): string {
  const s = normScale(p.scale);
  if (s === 'enum') return p.choices[Math.round(value)] ?? String(Math.round(value));
  if (s === 'bool') return value >= 0.5 ? 'On' : 'Off';
  const unit = p.unit ?? '';
  if (s === 'int') return `${Math.round(value)}${unit ? ' ' + unit : ''}`;
  const abs = Math.abs(value);
  if (unit === 'Hz' && abs >= 1000) return `${trimNum(value / 1000, abs >= 10000 ? 1 : 2)} kHz`;
  if (unit === 'ms' && abs >= 1000) return `${trimNum(value / 1000, 2)} s`;
  if (unit === '%') return `${trimNum(value, abs >= 10 ? 0 : 1)} %`;
  if (unit === 'dB') {
    if (value <= -120) return '-inf dB';
    return `${value > 0 ? '+' : ''}${trimNum(value, 1)} dB`;
  }
  if (unit === '' && p.min >= 0 && p.max <= 1) return `${Math.round(value * 100)} %`;
  const txt = trimNum(value, smartDigits(abs));
  return unit ? `${txt} ${unit}` : txt;
}

/** dBFS peak → 0..1 meter position (range -60..+6 dB). */
export function dbToMeter(db: number, floor = -60, ceil = 6): number {
  if (!Number.isFinite(db)) return 0;
  return clamp01((db - floor) / (ceil - floor));
}
