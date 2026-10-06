// High-rate data (telemetry, MIDI note activity) lives outside React state.
// Meters and the keyboard read it from a shared requestAnimationFrame loop and touch the DOM directly,
// so 30 Hz telemetry never causes a React re-render per frame.

import type { MidiNote, TelemetryEvent } from '../protocol/types';

export interface LiveState {
  telemetry: TelemetryEvent | null;
  /** performance.now() of last telemetry */
  telemetryAt: number;
  /** engine-reported active notes: note → velocity (0 = off) */
  engineNotes: Uint8Array;
  /** locally played notes (mouse / computer keyboard) */
  localNotes: Uint8Array;
  /** bumps whenever a note set changes */
  notesVersion: number;
}

export const live: LiveState = {
  telemetry: null,
  telemetryAt: 0,
  engineNotes: new Uint8Array(128),
  localNotes: new Uint8Array(128),
  notesVersion: 0,
};

export function pushTelemetry(t: TelemetryEvent, now = nowMs()): void {
  live.telemetry = t;
  live.telemetryAt = now;
}

export function pushMidi(notes: MidiNote[]): void {
  for (const n of notes) {
    if (n.note < 0 || n.note > 127) continue;
    live.engineNotes[n.note] = n.on ? Math.max(1, n.velocity | 0) : 0;
  }
  live.notesVersion++;
}

export function setLocalNote(note: number, velocity: number): void {
  if (note < 0 || note > 127) return;
  live.localNotes[note] = velocity;
  live.notesVersion++;
}

export function clearNotes(): void {
  live.engineNotes.fill(0);
  live.localNotes.fill(0);
  live.notesVersion++;
}

function nowMs(): number {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

// ─────────────── shared frame loop ───────────────

type FrameFn = (now: number, dt: number) => void;
const frameFns = new Set<FrameFn>();
let rafId = 0;
let last = 0;

function tick(now: number) {
  const dt = last ? Math.min(100, now - last) : 16;
  last = now;
  // snapshot: callbacks may (un)register during the frame
  for (const f of [...frameFns]) f(now, dt);
  rafId = frameFns.size ? requestAnimationFrame(tick) : 0;
  if (!rafId) last = 0;
}

/** Register a per-frame callback; returns unregister. */
export function onFrame(f: FrameFn): () => void {
  frameFns.add(f);
  if (!rafId && typeof requestAnimationFrame !== 'undefined') rafId = requestAnimationFrame(tick);
  return () => {
    frameFns.delete(f);
  };
}
