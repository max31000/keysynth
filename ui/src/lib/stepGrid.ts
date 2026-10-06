// Pure, immutable step-grid edits for drum patterns (docs/PRESETS.md "Patterns"). Every function returns a new
// Pattern (or the same object when nothing changed) with every row sized to `numSteps(p)`.

import type { Pattern, PatternTrack } from '../protocol/types';

export const MAX_TRACKS = 16;
export const MAX_STEPS = 256;
export const MAX_BARS = 4;
export const DEFAULT_VELOCITY = 100;
export const DENOMINATORS = [2, 4, 8, 16] as const;

/** GM drum notes handled by the `drums` kit (engine DrumKit::mapNote). */
export const DRUM_NOTES: { note: number; name: string }[] = [
  { note: 35, name: 'Kick 2' },
  { note: 36, name: 'Kick' },
  { note: 37, name: 'Rim' },
  { note: 38, name: 'Snare' },
  { note: 39, name: 'Clap' },
  { note: 40, name: 'Snare 2' },
  { note: 41, name: 'Floor Tom' },
  { note: 42, name: 'Closed Hat' },
  { note: 43, name: 'Floor Tom 2' },
  { note: 44, name: 'Pedal Hat' },
  { note: 45, name: 'Low Tom' },
  { note: 46, name: 'Open Hat' },
  { note: 47, name: 'Mid Tom' },
  { note: 48, name: 'Mid Tom 2' },
  { note: 49, name: 'Crash' },
  { note: 50, name: 'High Tom' },
  { note: 51, name: 'Ride' },
  { note: 52, name: 'China' },
  { note: 53, name: 'Ride Bell' },
  { note: 54, name: 'Tambourine' },
  { note: 55, name: 'Splash' },
  { note: 56, name: 'Cowbell' },
  { note: 57, name: 'Crash 2' },
  { note: 58, name: 'Cowbell Hi' },
  { note: 59, name: 'Ride 2' },
  { note: 69, name: 'Cabasa' },
  { note: 70, name: 'Shaker' },
];

export const drumNoteName = (note: number) => DRUM_NOTES.find((d) => d.note === note)?.name ?? `Note ${note}`;

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));
const clampVel = (v: number) => clamp(Math.round(Number.isFinite(v) ? v : 0), 0, 127);

export const stepsPerBar = (p: Pick<Pattern, 'time_sig' | 'steps_per_beat'>) => p.time_sig[0] * p.steps_per_beat;
export const numSteps = (p: Pick<Pattern, 'time_sig' | 'steps_per_beat' | 'bars'>) => p.bars * stepsPerBar(p);

/** Step position helpers for rendering (beat groups, bar lines). */
export const barOfStep = (p: Pattern, step: number) => Math.floor(step / stepsPerBar(p));
export const beatOfStep = (p: Pattern, step: number) => Math.floor(step / p.steps_per_beat);
export const isBeatStart = (p: Pattern, step: number) => step % p.steps_per_beat === 0;
export const isBarStart = (p: Pattern, step: number) => step % stepsPerBar(p) === 0;

const resize = (row: number[], n: number) => (row.length === n ? row : row.length > n ? row.slice(0, n) : [...row, ...new Array<number>(n - row.length).fill(0)]);

/** Clamp meter/bars/rows like the engine (Pattern::normalize). */
export function normalizePattern(p: Pattern): Pattern {
  const num = clamp(Math.round(p.time_sig?.[0] ?? 4), 1, 16);
  const den = (DENOMINATORS as readonly number[]).includes(p.time_sig?.[1]) ? p.time_sig[1] : 4;
  let spb = clamp(Math.round(p.steps_per_beat || 4), 1, 8);
  let bars = clamp(Math.round(p.bars || 1), 1, MAX_BARS);
  while (bars > 1 && bars * num * spb > MAX_STEPS) bars--;
  while (spb > 1 && bars * num * spb > MAX_STEPS) spb--;
  const n = bars * num * spb;
  return {
    ...p,
    format: 1,
    time_sig: [num, den],
    steps_per_beat: spb,
    bars,
    swing: clamp(p.swing ?? 0, 0, 1),
    accent_amount: clamp(p.accent_amount ?? 0.5, 0, 1),
    tracks: p.tracks.slice(0, MAX_TRACKS).map((t) => ({ ...t, steps: resize(t.steps.map(clampVel), n) })),
    accent: resize((p.accent ?? []).map((a) => (a ? 1 : 0)), n),
  };
}

function mapTrack(p: Pattern, track: number, f: (t: PatternTrack) => PatternTrack): Pattern {
  const t = p.tracks[track];
  if (!t) return p;
  const nt = f(t);
  if (nt === t) return p;
  const tracks = p.tracks.slice();
  tracks[track] = nt;
  return { ...p, tracks };
}

export function setStepVelocity(p: Pattern, track: number, step: number, velocity: number): Pattern {
  if (step < 0 || step >= numSteps(p)) return p;
  const v = clampVel(velocity);
  return mapTrack(p, track, (t) => {
    if (t.steps[step] === v) return t;
    const steps = resize(t.steps, numSteps(p)).slice();
    steps[step] = v;
    return { ...t, steps };
  });
}

/** Click: off → `velocity`, on → off. */
export function toggleStep(p: Pattern, track: number, step: number, velocity = DEFAULT_VELOCITY): Pattern {
  const cur = p.tracks[track]?.steps[step] ?? 0;
  return setStepVelocity(p, track, step, cur > 0 ? 0 : velocity);
}

/** Vertical drag: up = louder. 1 velocity unit per pixel, result 1..127 (dragging never turns a step off). */
export function velocityFromDrag(startVelocity: number, dyPixels: number): number {
  return clamp(Math.round((startVelocity > 0 ? startVelocity : DEFAULT_VELOCITY) - dyPixels), 1, 127);
}

export function toggleAccent(p: Pattern, step: number): Pattern {
  if (step < 0 || step >= numSteps(p)) return p;
  const accent = resize(p.accent, numSteps(p)).slice();
  accent[step] = accent[step] ? 0 : 1;
  return { ...p, accent };
}

export const toggleMute = (p: Pattern, track: number) => mapTrack(p, track, (t) => ({ ...t, mute: !t.mute }));

export function setTrackNote(p: Pattern, track: number, note: number): Pattern {
  const n = clamp(Math.round(note), 0, 127);
  return mapTrack(p, track, (t) => (t.note === n ? t : { ...t, note: n, name: drumNoteName(n) }));
}

export function addTrack(p: Pattern, note = 36): Pattern {
  if (p.tracks.length >= MAX_TRACKS) return p;
  return { ...p, tracks: [...p.tracks, { name: drumNoteName(note), note, mute: false, steps: new Array<number>(numSteps(p)).fill(0) }] };
}

export function removeTrack(p: Pattern, track: number): Pattern {
  if (!p.tracks[track]) return p;
  return { ...p, tracks: p.tracks.filter((_, i) => i !== track) };
}

export function clearSteps(p: Pattern): Pattern {
  const n = numSteps(p);
  return { ...p, tracks: p.tracks.map((t) => ({ ...t, steps: new Array<number>(n).fill(0) })), accent: new Array<number>(n).fill(0) };
}

/** Meter change keeps steps by index (same as the engine's transport `time_sig`). */
export function setTimeSig(p: Pattern, num: number, den: number): Pattern {
  return normalizePattern({ ...p, time_sig: [num, den] });
}

/** Bars change: extra bars repeat the first ones (so 1 → 2 bars duplicates the groove), fewer bars truncate. */
export function setBars(p: Pattern, bars: number): Pattern {
  const next = normalizePattern({ ...p, bars });
  if (next.bars <= p.bars) return next;
  const old = numSteps(p);
  const rep = (row: number[]) => row.map((v, i) => (i < old ? v : (row[i % old] ?? 0)));
  return {
    ...next,
    tracks: next.tracks.map((t, k) => ({ ...t, steps: rep(resize(p.tracks[k]!.steps, numSteps(next))) })),
    accent: rep(resize(p.accent, numSteps(next))),
  };
}

/** Resolution change re-times the hits: step i lands on round(i * new / old) (collisions keep the louder hit). */
export function setStepsPerBeat(p: Pattern, spb: number): Pattern {
  const next = normalizePattern({ ...p, steps_per_beat: spb });
  if (next.steps_per_beat === p.steps_per_beat) return next;
  const ratio = next.steps_per_beat / p.steps_per_beat;
  const n = numSteps(next);
  const remap = (row: number[], max: boolean) => {
    const out = new Array<number>(n).fill(0);
    row.forEach((v, i) => {
      if (!v) return;
      const j = Math.round(i * ratio);
      if (j < n) out[j] = max ? Math.max(out[j]!, v) : 1;
    });
    return out;
  };
  return {
    ...next,
    tracks: p.tracks.map((t) => ({ ...t, steps: remap(t.steps, true) })),
    accent: remap(p.accent, false),
  };
}

export function emptyPattern(): Pattern {
  const tracks: [string, number][] = [
    ['Kick', 36], ['Snare', 38], ['Clap', 39], ['Closed Hat', 42], ['Open Hat', 46], ['Low Tom', 45], ['High Tom', 50], ['Crash', 49],
  ];
  return normalizePattern({
    format: 1,
    name: 'Empty',
    time_sig: [4, 4],
    steps_per_beat: 4,
    bars: 1,
    swing: 0,
    accent_amount: 0.5,
    tracks: tracks.map(([name, note]) => ({ name, note, mute: false, steps: [] })),
    accent: [],
  });
}

/** Count of active steps (for UI badges / tests). */
export const hitCount = (p: Pattern) => p.tracks.reduce((a, t) => a + t.steps.filter((v) => v > 0).length, 0);
