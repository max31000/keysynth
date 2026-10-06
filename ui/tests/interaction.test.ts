import { describe, expect, it } from 'vitest';
import { ComputerKeyboard } from '../src/lib/computerKeys';
import { insertionIndex, moveTarget, type SlotRect } from '../src/lib/reorder';
import { dragNorm, formatValue, fromNorm, toNorm } from '../src/lib/paramMath';
import type { ParamSpec } from '../src/protocol/types';

describe('computer keyboard', () => {
  it('maps A-row to C{octave} and tracks held keys', () => {
    const k = new ComputerKeyboard();
    expect(k.keyDown('KeyA')).toEqual({ type: 'on', note: 60, velocity: 96 });
    expect(k.keyDown('KeyW')).toMatchObject({ type: 'on', note: 61 });
    expect(k.keyDown('KeyA')).toBeNull(); // already held
    expect(k.keyDown('KeyA', true)).toBeNull(); // auto-repeat
    expect(k.heldCount).toBe(2);
    expect(k.keyUp('KeyA')).toEqual({ type: 'off', note: 60 });
    expect(k.keyUp('KeyA')).toBeNull();
    expect(k.keyDown('KeyQ')).toBeNull(); // unmapped
  });

  it('octave change mid-hold releases the originally played note', () => {
    const k = new ComputerKeyboard();
    k.keyDown('KeyD'); // E4 = 64
    expect(k.keyDown('KeyZ')).toEqual({ type: 'octave', octave: 3 });
    expect(k.keyDown('KeyD')).toBeNull(); // still held, no retrigger
    expect(k.keyUp('KeyD')).toEqual({ type: 'off', note: 64 });
    expect(k.keyDown('KeyD')).toMatchObject({ note: 52 });
  });

  it('octave and velocity are clamped; repeated Z steps accumulate', () => {
    const k = new ComputerKeyboard();
    for (let i = 0; i < 10; i++) k.keyDown('KeyZ');
    expect(k.octave).toBe(1);
    for (let i = 0; i < 10; i++) k.keyDown('KeyX');
    expect(k.octave).toBe(7);
    expect(k.keyDown('KeyV')).toEqual({ type: 'velocity', velocity: 112 });
    k.keyDown('KeyV');
    k.keyDown('KeyV');
    expect(k.velocity).toBe(127);
    for (let i = 0; i < 9; i++) k.keyDown('KeyC');
    expect(k.velocity).toBe(32);
  });

  it('releaseAll returns every sounding note once', () => {
    const k = new ComputerKeyboard();
    k.keyDown('KeyA');
    k.keyDown('KeyG');
    expect(k.releaseAll().sort()).toEqual([60, 67]);
    expect(k.heldCount).toBe(0);
    expect(k.keyUp('KeyA')).toBeNull();
  });
});

describe('knob drag math', () => {
  const p: ParamSpec = {
    id: 'c', name: 'C', group: '', unit: 'Hz', min: 20, max: 20000, def: 1000,
    scale: 'log', skewCentre: 1000, flags: 0, choices: [],
  };
  it('200 px = full range, shift = 2000 px, clamped', () => {
    expect(dragNorm(0.5, -100, false)).toBeCloseTo(1);
    expect(dragNorm(0.5, -100, true)).toBeCloseTo(0.55);
    expect(dragNorm(0.5, 1000, false)).toBe(0);
  });
  it('fine drag from centre moves the value a little in plain units', () => {
    const n = dragNorm(toNorm(p, 1000), -20, true); // +0.01
    const v = fromNorm(p, n);
    expect(v).toBeGreaterThan(1000);
    expect(v).toBeLessThan(1100);
  });
  it('incremental steps accumulate like one long drag', () => {
    let n = 0.2;
    for (let i = 0; i < 10; i++) n = dragNorm(n, -5, true);
    expect(n).toBeCloseTo(dragNorm(0.2, -50, true), 9);
  });
  it('enum without choices does not blow up', () => {
    const e: ParamSpec = { ...p, scale: 'enum', min: 0, max: 3, def: 0, choices: [], unit: '', skewCentre: 0 };
    expect(fromNorm(e, 1)).toBe(3);
    expect(toNorm(e, 3)).toBe(1);
    expect(formatValue(e, 2)).toBe('2');
    const empty: ParamSpec = { ...e, max: 0 };
    expect(fromNorm(empty, 0.7)).toBe(0);
    expect(toNorm(empty, 0)).toBe(0);
  });
});

describe('fx reorder', () => {
  const row = (n: number, top = 0): SlotRect[] =>
    Array.from({ length: n }, (_, i) => ({ left: i * 100, top, width: 90, height: 30 }));

  it('horizontal insertion index by midpoint', () => {
    const r = row(3);
    expect(insertionIndex(r, 10, 15, false)).toBe(0);
    expect(insertionIndex(r, 50, 15, false)).toBe(1);
    expect(insertionIndex(r, 160, 15, false)).toBe(2);
    expect(insertionIndex(r, 400, 15, false)).toBe(3);
  });

  it('wrapped rows: slots on earlier rows count as before', () => {
    const r = [...row(2, 0), ...row(2, 40)]; // two rows of two
    expect(insertionIndex(r, 10, 55, false)).toBe(2); // start of second row
    expect(insertionIndex(r, 160, 55, false)).toBe(4);
    expect(insertionIndex(r, 160, 15, false)).toBe(2); // end of first row
  });

  it('vertical uses y midpoints', () => {
    const r: SlotRect[] = [0, 1, 2].map((i) => ({ left: 0, top: i * 40, width: 100, height: 30 }));
    expect(insertionIndex(r, 0, 5, true)).toBe(0);
    expect(insertionIndex(r, 0, 20, true)).toBe(1);
    expect(insertionIndex(r, 0, 999, true)).toBe(3);
  });

  it('moveTarget converts insertion index to post-removal position', () => {
    expect(moveTarget(0, 3)).toBe(2); // first → end of 3
    expect(moveTarget(2, 0)).toBe(0);
    expect(moveTarget(1, 1)).toBeNull(); // dropped onto itself
    expect(moveTarget(1, 2)).toBeNull();
    expect(moveTarget(-1, 0)).toBeNull();
  });
});
