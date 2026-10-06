import { describe, expect, it } from 'vitest';
import type { ParamSpec } from '../src/protocol/types';
import {
  dragValue,
  formatValue,
  fromNorm,
  normScale,
  quantize,
  skewExponent,
  toNorm,
  wheelValue,
} from '../src/lib/paramMath';

const spec = (o: Partial<ParamSpec>): ParamSpec => ({
  id: 'p',
  name: 'P',
  group: 'G',
  unit: '',
  min: 0,
  max: 1,
  def: 0,
  scale: 'linear',
  skewCentre: 0,
  flags: 0,
  choices: [],
  ...o,
});

describe('linear', () => {
  const p = spec({ min: -10, max: 30 });
  it('maps ends and middle', () => {
    expect(toNorm(p, -10)).toBe(0);
    expect(toNorm(p, 30)).toBe(1);
    expect(toNorm(p, 10)).toBeCloseTo(0.5);
    expect(fromNorm(p, 0.25)).toBeCloseTo(0);
  });
  it('clamps out of range', () => {
    expect(toNorm(p, 100)).toBe(1);
    expect(fromNorm(p, -1)).toBe(-10);
  });
});

describe('log', () => {
  it('geometric mean at centre when skewCentre = 0', () => {
    const p = spec({ min: 20, max: 20000, scale: 'log' });
    expect(fromNorm(p, 0.5)).toBeCloseTo(Math.sqrt(20 * 20000), 6);
    expect(toNorm(p, 632.4555)).toBeCloseTo(0.5, 4);
  });
  it('skewCentre lands exactly at 0.5', () => {
    const p = spec({ min: 20, max: 20000, scale: 'log', skewCentre: 1000 });
    expect(toNorm(p, 1000)).toBeCloseTo(0.5, 9);
    expect(fromNorm(p, 0.5)).toBeCloseTo(1000, 6);
    expect(fromNorm(p, 0)).toBe(20);
    expect(fromNorm(p, 1)).toBeCloseTo(20000, 6);
  });
  it('round-trips', () => {
    const p = spec({ min: 1, max: 10000, scale: 'log', skewCentre: 500 });
    for (const v of [1, 3.3, 47, 500, 999, 7777, 10000]) expect(fromNorm(p, toNorm(p, v))).toBeCloseTo(v, 6);
  });
  it('is monotonic', () => {
    const p = spec({ min: 0.1, max: 20, scale: 'log', skewCentre: 2 });
    let prev = -Infinity;
    for (let n = 0; n <= 1; n += 0.05) {
      const v = fromNorm(p, n);
      expect(v).toBeGreaterThan(prev);
      prev = v;
    }
  });
  it('degenerate skew centre falls back to linear exponent', () => {
    expect(skewExponent(0, 1, 0.5)).toBeCloseTo(1);
    expect(skewExponent(0, 1, 2)).toBe(1);
  });
  it('log with min <= 0 and no skew behaves linearly', () => {
    const p = spec({ min: 0, max: 10, scale: 'log' });
    expect(fromNorm(p, 0.5)).toBeCloseTo(5);
  });
});

describe('int', () => {
  const p = spec({ min: -24, max: 24, scale: 'int' });
  it('rounds to integers', () => {
    expect(fromNorm(p, 0.51)).toBe(0);
    expect(fromNorm(p, 0.52)).toBe(1);
    expect(quantize(p, 3.6)).toBe(4);
  });
  it('wheel steps by one', () => {
    expect(wheelValue(p, 0, 1, false)).toBe(1);
    expect(wheelValue(p, 24, 1, false)).toBe(24);
    expect(wheelValue(p, 0, -1, true)).toBe(-1);
  });
});

describe('enum', () => {
  const p = spec({ scale: 'enum', choices: ['Saw', 'Pulse', 'Tri', 'Sine'], min: 0, max: 3 });
  it('index range from choices', () => {
    expect(toNorm(p, 0)).toBe(0);
    expect(toNorm(p, 3)).toBe(1);
    expect(fromNorm(p, 0.4)).toBe(1);
    expect(fromNorm(p, 0.6)).toBe(2);
  });
  it('formats with labels', () => {
    expect(formatValue(p, 2)).toBe('Tri');
  });
  it('accepts capitalised scale names', () => {
    expect(normScale('Enum')).toBe('enum');
    expect(fromNorm({ ...p, scale: 'Enum' as never }, 1)).toBe(3);
  });
});

describe('bool', () => {
  const p = spec({ scale: 'bool' });
  it('snaps', () => {
    expect(quantize(p, 0.7)).toBe(1);
    expect(quantize(p, 0.2)).toBe(0);
    expect(formatValue(p, 1)).toBe('On');
  });
});

describe('drag & format', () => {
  it('200px drag covers the range; shift is 10x finer', () => {
    const p = spec({ min: 0, max: 100 });
    expect(dragValue(p, 0, -200, false)).toBeCloseTo(100);
    expect(dragValue(p, 0, -200, true)).toBeCloseTo(10);
  });
  it('formats units', () => {
    expect(formatValue(spec({ min: 20, max: 20000, unit: 'Hz' }), 2400)).toBe('2.4 kHz');
    expect(formatValue(spec({ min: 1, max: 10000, unit: 'ms' }), 1500)).toBe('1.5 s');
    expect(formatValue(spec({ min: -60, max: 6, unit: 'dB' }), 3)).toBe('+3 dB');
    expect(formatValue(spec({}), 0.25)).toBe('25 %');
    expect(formatValue(spec({ min: -24, max: 24, unit: 'st', scale: 'int' }), -7)).toBe('-7 st');
  });
});
