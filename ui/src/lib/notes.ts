const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/** MIDI note → name, C4 = 60 (PRESETS.md). */
export function noteName(n: number): string {
  return `${NAMES[((n % 12) + 12) % 12]}${Math.floor(n / 12) - 1}`;
}

export const isBlack = (n: number) => [1, 3, 6, 8, 10].includes(((n % 12) + 12) % 12);

export const PIANO_LO = 21; // A0
export const PIANO_HI = 108; // C8

export interface KeyGeom {
  note: number;
  black: boolean;
  /** x in white-key units */
  x: number;
  w: number;
}

/** Key geometry for a range; white keys are 1 unit wide, black keys 0.6, offset for a natural look. */
export function keyLayout(lo: number, hi: number): { keys: KeyGeom[]; whiteCount: number } {
  const keys: KeyGeom[] = [];
  let wx = 0;
  const blackOffset: Record<number, number> = { 1: -0.36, 3: -0.24, 6: -0.38, 8: -0.3, 10: -0.22 };
  for (let n = lo; n <= hi; n++) {
    if (isBlack(n)) {
      keys.push({ note: n, black: true, x: wx + (blackOffset[n % 12] ?? -0.3), w: 0.6 });
    } else {
      keys.push({ note: n, black: false, x: wx, w: 1 });
      wx++;
    }
  }
  return { keys, whiteCount: wx };
}
