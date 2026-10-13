// `chord` MIDI processor helpers (ARCHITECTURE §17): learned memory (hidden params `mem_count`, `mem_1`…`mem_8`,
// semitones above the lowest learned note) and the held-notes readout (`held_count`, `held_1`…`held_8`).

import type { ParamValues } from '../protocol/types';

export const CHORD_MEM_MAX = 8;
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/** Learned intervals (empty = nothing learned). */
export function memoryIntervals(params: ParamValues | undefined): number[] {
  if (!params) return [];
  const n = Math.max(0, Math.min(CHORD_MEM_MAX, Math.round(params.mem_count ?? 0)));
  const out: number[] = [];
  for (let i = 1; i <= n; i++) out.push(Math.round(params[`mem_${i}`] ?? 0));
  return out;
}

/** Intervals as note names over C ("C E G B"); one octave above shows as the same name. */
export function intervalNames(intervals: number[]): string {
  return intervals.map((i) => NAMES[((i % 12) + 12) % 12]).join(' ');
}

/** Notes held on the processor's input, lowest first (telemetry readout). */
export function heldNotes(readout: Record<string, number> | undefined): number[] {
  if (!readout) return [];
  const n = Math.max(0, Math.min(CHORD_MEM_MAX, Math.round(readout.held_count ?? 0)));
  const out: number[] = [];
  for (let i = 1; i <= n; i++) {
    const v = Math.round(readout[`held_${i}`] ?? -1);
    if (v >= 0 && v <= 127) out.push(v);
  }
  return out;
}
