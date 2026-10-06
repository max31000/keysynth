import { describe, expect, it } from 'vitest';
import {
  addTrack,
  clearSteps,
  emptyPattern,
  hitCount,
  isBarStart,
  isBeatStart,
  normalizePattern,
  numSteps,
  removeTrack,
  setBars,
  setStepVelocity,
  setStepsPerBeat,
  setTimeSig,
  setTrackNote,
  toggleAccent,
  toggleMute,
  toggleStep,
  velocityFromDrag,
} from '../src/lib/stepGrid';
import { parsePatternJson, factoryPatterns } from '../mock/patterns';
import type { Pattern } from '../src/protocol/types';

const base = (): Pattern => emptyPattern();

describe('step grid state', () => {
  it('empty pattern: 8 tracks x 16 steps, all rows sized', () => {
    const p = base();
    expect(numSteps(p)).toBe(16);
    expect(p.tracks).toHaveLength(8);
    for (const t of p.tracks) expect(t.steps).toHaveLength(16);
    expect(p.accent).toHaveLength(16);
    expect(hitCount(p)).toBe(0);
  });

  it('toggle / velocity edits are immutable and clamped', () => {
    const p = base();
    const a = toggleStep(p, 0, 4);
    expect(a).not.toBe(p);
    expect(p.tracks[0]!.steps[4]).toBe(0);
    expect(a.tracks[0]!.steps[4]).toBe(100);
    expect(a.tracks[1]).toBe(p.tracks[1]); // untouched rows shared
    expect(toggleStep(a, 0, 4).tracks[0]!.steps[4]).toBe(0);
    expect(setStepVelocity(a, 0, 4, 300).tracks[0]!.steps[4]).toBe(127);
    expect(setStepVelocity(a, 0, 4, -3).tracks[0]!.steps[4]).toBe(0);
    expect(setStepVelocity(a, 0, 99, 50)).toBe(a); // out of range
    expect(setStepVelocity(a, 42, 1, 50)).toBe(a); // no such track
    expect(setStepVelocity(a, 0, 4, 100)).toBe(a); // unchanged
  });

  it('velocity drag: up is louder, never below 1', () => {
    expect(velocityFromDrag(100, -20)).toBe(120);
    expect(velocityFromDrag(100, 500)).toBe(1);
    expect(velocityFromDrag(0, -10)).toBe(110); // off step starts from the default
    expect(velocityFromDrag(120, -50)).toBe(127);
  });

  it('accent, mute, note, add/remove track', () => {
    let p = toggleAccent(base(), 0);
    expect(p.accent[0]).toBe(1);
    expect(toggleAccent(p, 0).accent[0]).toBe(0);
    p = toggleMute(p, 2);
    expect(p.tracks[2]!.mute).toBe(true);
    p = setTrackNote(p, 2, 56);
    expect(p.tracks[2]).toMatchObject({ note: 56, name: 'Cowbell' });
    p = addTrack(p, 54);
    expect(p.tracks).toHaveLength(9);
    expect(p.tracks[8]!.steps).toHaveLength(16);
    p = removeTrack(p, 0);
    expect(p.tracks).toHaveLength(8);
    expect(p.tracks[0]!.name).toBe('Snare');
    let full = base();
    for (let i = 0; i < 20; i++) full = addTrack(full);
    expect(full.tracks).toHaveLength(16);
  });

  it('meter change keeps steps by index (7/4 = 28 steps)', () => {
    const p = setStepVelocity(setStepVelocity(base(), 0, 0, 100), 0, 15, 90);
    const q = setTimeSig(p, 7, 4);
    expect(numSteps(q)).toBe(28);
    expect(q.tracks[0]!.steps[0]).toBe(100);
    expect(q.tracks[0]!.steps[15]).toBe(90);
    expect(q.tracks[0]!.steps[27]).toBe(0);
    const r = setTimeSig(q, 3, 4);
    expect(numSteps(r)).toBe(12);
    expect(r.tracks[0]!.steps).toHaveLength(12);
    expect(setTimeSig(p, 4, 3).time_sig).toEqual([4, 4]); // invalid denominator → 4
    expect(isBarStart(q, 0) && isBeatStart(q, 4) && !isBeatStart(q, 5)).toBe(true);
  });

  it('bars: growing repeats the groove, shrinking truncates', () => {
    const p = toggleAccent(setStepVelocity(base(), 0, 2, 77), 2);
    const two = setBars(p, 2);
    expect(numSteps(two)).toBe(32);
    expect(two.tracks[0]!.steps[2]).toBe(77);
    expect(two.tracks[0]!.steps[18]).toBe(77);
    expect(two.accent[18]).toBe(1);
    expect(numSteps(setBars(two, 1))).toBe(16);
    expect(setBars(p, 9).bars).toBe(4);
  });

  it('steps per beat change re-times hits', () => {
    let p = base();
    for (const s of [0, 2, 4, 6]) p = setStepVelocity(p, 3, s, 100); // 8ths in 16ths
    const eighths = setStepsPerBeat(p, 2);
    expect(numSteps(eighths)).toBe(8);
    expect(eighths.tracks[3]!.steps.slice(0, 4)).toEqual([100, 100, 100, 100]);
    const back = setStepsPerBeat(eighths, 4);
    expect(back.tracks[3]!.steps.slice(0, 8)).toEqual([100, 0, 100, 0, 100, 0, 100, 0]);
  });

  it('normalize clamps like the engine (<= 256 steps, <= 16 tracks)', () => {
    const p = normalizePattern({ ...base(), time_sig: [16, 4], steps_per_beat: 8, bars: 4 });
    expect(numSteps(p)).toBeLessThanOrEqual(256);
    expect(clearSteps(toggleStep(base(), 0, 0)).tracks[0]!.steps[0]).toBe(0);
  });
});

describe('pattern files (mock parser)', () => {
  it('parses string rows, velocity chars, kit names and infers bars', () => {
    const p = parsePatternJson({
      name: 'T',
      time_sig: [3, 4],
      kit: 'industrial',
      tracks: [{ name: 'K', note: 36, steps: 'x... X... o... | 9..1 .... ....' }],
      accent: 'x',
    });
    expect(p.kit).toBe(3);
    expect(p.bars).toBe(2);
    expect(numSteps(p)).toBe(24);
    expect(p.tracks[0]!.steps.slice(0, 16).filter((v) => v > 0)).toEqual([100, 127, 50, 127, 14]);
    expect(p.accent[0]).toBe(1);
  });

  it('every factory pattern loads (>= 10, incl. 7/4)', () => {
    const all = factoryPatterns();
    expect(all.length).toBeGreaterThanOrEqual(10);
    const money = all.find((p) => p.entry.path.endsWith('money-7-4.json'))!;
    expect(money.pattern.time_sig).toEqual([7, 4]);
    expect(numSteps(money.pattern)).toBe(28);
    for (const p of all) expect(hitCount(p.pattern)).toBeGreaterThan(4);
  });
});
