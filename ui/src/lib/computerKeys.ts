// Computer-keyboard playing: two rows (A-row white keys, Q-row black keys), Z/X octave, C/V velocity.
// Pure state machine so it can be unit-tested; the Keyboard component wires it to DOM events.

/** KeyboardEvent.code → semitone from the octave's C. */
export const KEY_MAP: Record<string, number> = {
  KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11,
  KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16, Quote: 17,
};
export const VELOCITIES = [32, 64, 96, 112, 127];
export const OCTAVE_MIN = 1;
export const OCTAVE_MAX = 7;

export type KeyAction =
  | { type: 'on'; note: number; velocity: number }
  | { type: 'off'; note: number }
  | { type: 'octave'; octave: number }
  | { type: 'velocity'; velocity: number };

export class ComputerKeyboard {
  octave = 4; // A-key plays C{octave}
  velocity = 96;
  /** code → note that is sounding, so octave changes mid-hold release the right note */
  private held = new Map<string, number>();

  setOctave(o: number): number {
    this.octave = Math.min(OCTAVE_MAX, Math.max(OCTAVE_MIN, o));
    return this.octave;
  }

  stepVelocity(dir: number): number {
    const i = VELOCITIES.indexOf(this.velocity);
    const base = i < 0 ? VELOCITIES.findIndex((v) => v >= this.velocity) : i;
    this.velocity = VELOCITIES[Math.min(VELOCITIES.length - 1, Math.max(0, base + dir))]!;
    return this.velocity;
  }

  noteFor(code: string): number | undefined {
    const semi = KEY_MAP[code];
    return semi === undefined ? undefined : (this.octave + 1) * 12 + semi;
  }

  /** Auto-repeat and already-held keys produce nothing. */
  keyDown(code: string, repeat = false): KeyAction | null {
    if (repeat) return null;
    switch (code) {
      case 'KeyZ':
        return { type: 'octave', octave: this.setOctave(this.octave - 1) };
      case 'KeyX':
        return { type: 'octave', octave: this.setOctave(this.octave + 1) };
      case 'KeyC':
        return { type: 'velocity', velocity: this.stepVelocity(-1) };
      case 'KeyV':
        return { type: 'velocity', velocity: this.stepVelocity(1) };
    }
    if (this.held.has(code)) return null;
    const note = this.noteFor(code);
    if (note === undefined || note > 127) return null;
    this.held.set(code, note);
    return { type: 'on', note, velocity: this.velocity };
  }

  keyUp(code: string): KeyAction | null {
    const note = this.held.get(code);
    if (note === undefined) return null;
    this.held.delete(code);
    return { type: 'off', note };
  }

  /** Release everything (window blur / tab hidden). Returns the notes to turn off. */
  releaseAll(): number[] {
    const notes = [...new Set(this.held.values())];
    this.held.clear();
    return notes;
  }

  get heldCount(): number {
    return this.held.size;
  }
}
