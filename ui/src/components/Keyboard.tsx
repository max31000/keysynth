import { useEffect, useMemo, useRef, useState, type PointerEvent } from 'react';
import { actions } from '../store';
import { live, onFrame, setLocalNote } from '../store/liveBus';
import { keyLayout, noteName, PIANO_HI, PIANO_LO } from '../lib/notes';
import { ComputerKeyboard, KEY_MAP, VELOCITIES } from '../lib/computerKeys';

const CODE_LABEL: Record<string, string> = { Semicolon: ';', Quote: "'" };

const isTyping = (t: EventTarget | null) =>
  t instanceof HTMLElement && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);

export function Keyboard() {
  const { keys, whiteCount } = useMemo(() => keyLayout(PIANO_LO, PIANO_HI), []);
  const keyEls = useRef<(HTMLDivElement | null)[]>([]);
  const tracker = useRef(new ComputerKeyboard()).current;
  const [octave, setOctaveState] = useState(tracker.octave); // A-key = C{octave}
  const [velocity, setVelocityState] = useState(tracker.velocity);
  const setOctave = (o: number) => setOctaveState(tracker.setOctave(o));
  const setVelocity = (v: number) => {
    tracker.velocity = v;
    setVelocityState(v);
  };
  const mouseNote = useRef<number | null>(null);

  const play = (note: number, vel: number) => {
    if (note < PIANO_LO || note > PIANO_HI) return;
    setLocalNote(note, vel);
    actions().note(true, note, vel);
  };
  const release = (note: number) => {
    setLocalNote(note, 0);
    actions().note(false, note, 0);
  };

  // highlight from engine + local notes, on the frame loop
  useEffect(() => {
    let seen = -1;
    return onFrame(() => {
      if (live.notesVersion === seen) return;
      seen = live.notesVersion;
      for (let n = PIANO_LO; n <= PIANO_HI; n++) {
        const el = keyEls.current[n];
        if (!el) continue;
        const v = Math.max(live.engineNotes[n]!, live.localNotes[n]!);
        el.classList.toggle('down', v > 0);
        if (v > 0) el.style.setProperty('--vel', String(v / 127));
      }
    });
  }, []);

  // computer keyboard
  useEffect(() => {
    const down = (e: KeyboardEvent) => {
      if (e.ctrlKey || e.metaKey || e.altKey || isTyping(e.target)) return;
      if (e.repeat && KEY_MAP[e.code] !== undefined) {
        e.preventDefault(); // swallow auto-repeat of held note keys
        return;
      }
      const a = tracker.keyDown(e.code, e.repeat);
      if (!a) return;
      e.preventDefault();
      if (a.type === 'on') play(a.note, a.velocity);
      else if (a.type === 'octave') setOctaveState(a.octave);
      else if (a.type === 'velocity') setVelocityState(a.velocity);
    };
    const up = (e: KeyboardEvent) => {
      const a = tracker.keyUp(e.code);
      if (a?.type === 'off') release(a.note);
    };
    const releaseAll = () => {
      for (const n of tracker.releaseAll()) release(n);
      if (mouseNote.current !== null) release(mouseNote.current);
      mouseNote.current = null;
    };
    const onVisibility = () => {
      if (document.visibilityState === 'hidden') releaseAll();
    };
    window.addEventListener('keydown', down);
    window.addEventListener('keyup', up);
    window.addEventListener('blur', releaseAll);
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      window.removeEventListener('keydown', down);
      window.removeEventListener('keyup', up);
      window.removeEventListener('blur', releaseAll);
      document.removeEventListener('visibilitychange', onVisibility);
    };
    // play/release only touch stable module-level functions
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const up = () => {
      if (mouseNote.current !== null) release(mouseNote.current);
      mouseNote.current = null;
    };
    window.addEventListener('pointerup', up);
    return () => window.removeEventListener('pointerup', up);
  }, []);

  const velFromY = (e: PointerEvent, el: HTMLElement) => {
    const r = el.getBoundingClientRect();
    return Math.round(30 + 97 * Math.min(1, Math.max(0, (e.clientY - r.top) / r.height)));
  };
  const onKeyDown = (note: number) => (e: PointerEvent) => {
    if (e.button !== 0) return;
    e.preventDefault();
    (e.currentTarget as HTMLElement).releasePointerCapture?.(e.pointerId);
    mouseNote.current = note;
    play(note, velFromY(e, e.currentTarget as HTMLElement));
  };
  const onKeyEnter = (note: number) => (e: PointerEvent) => {
    if (mouseNote.current === null || !(e.buttons & 1) || mouseNote.current === note) return;
    release(mouseNote.current);
    mouseNote.current = note;
    play(note, velFromY(e, e.currentTarget as HTMLElement));
  };

  const base = (octave + 1) * 12;
  const codeFor = new Map<number, string>();
  for (const [code, semi] of Object.entries(KEY_MAP)) {
    codeFor.set(base + semi, CODE_LABEL[code] ?? code.replace('Key', ''));
  }
  const unit = 100 / whiteCount;
  const lowX = keys.find((k) => k.note === base)?.x ?? 0;
  const highKey = keys.find((k) => k.note === Math.min(PIANO_HI, base + 17));
  const highX = highKey ? highKey.x + highKey.w : whiteCount;

  return (
    <footer className="kbd-dock">
      <div className="kbd-controls">
        <div className="kbd-group">
          <span className="panel-label">Octave</span>
          <button type="button" className="btn small" onClick={() => setOctave(octave - 1)} title="Octave down (Z)">
            Z −
          </button>
          <span className="mono kbd-oct">C{octave}</span>
          <button type="button" className="btn small" onClick={() => setOctave(octave + 1)} title="Octave up (X)">
            X +
          </button>
        </div>
        <div className="kbd-group">
          <span className="panel-label">Velocity</span>
          <div className="segmented">
            {VELOCITIES.map((v) => (
              <button type="button" key={v} className={v === velocity ? 'on' : ''} onClick={() => setVelocity(v)}>
                {v}
              </button>
            ))}
          </div>
          <span className="hint">C / V</span>
        </div>
        <div className="hint kbd-help">Play with A W S E D F T G Y H U J K O L P ; ' — mouse: lower on the key = louder</div>
      </div>
      <div className="kbd" role="group" aria-label="On-screen keyboard A0 to C8">
        <div className="kbd-range" style={{ left: `${lowX * unit}%`, width: `${(highX - lowX) * unit}%` }} />
        {keys.map((k) => (
          <div
            key={k.note}
            ref={(el) => void (keyEls.current[k.note] = el)}
            className={`key ${k.black ? 'black' : 'white'}`}
            style={{ left: `${k.x * unit}%`, width: `${k.w * unit}%` }}
            onPointerDown={onKeyDown(k.note)}
            onPointerEnter={onKeyEnter(k.note)}
            data-note={k.note}
            title={noteName(k.note)}
          >
            {codeFor.has(k.note) && <span className="key-code">{codeFor.get(k.note)}</span>}
            {!k.black && k.note % 12 === 0 && <span className="key-label">{noteName(k.note)}</span>}
          </div>
        ))}
      </div>
    </footer>
  );
}
