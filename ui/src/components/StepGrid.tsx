import { memo, useRef, type KeyboardEvent, type PointerEvent } from 'react';
import type { Pattern } from '../protocol/types';
import {
  DEFAULT_VELOCITY,
  DRUM_NOTES,
  MAX_TRACKS,
  addTrack,
  isBarStart,
  isBeatStart,
  numSteps,
  removeTrack,
  setStepVelocity,
  setTrackNote,
  toggleAccent,
  toggleMute,
  velocityFromDrag,
} from '../lib/stepGrid';

type Edit = (fn: (p: Pattern) => Pattern) => void;

interface Props {
  pattern: Pattern;
  /** step currently playing (-1 = none) */
  playStep: number;
  onEdit: Edit;
}

type Gesture =
  | { kind: 'paint'; value: number; done: Set<string> }
  | { kind: 'velocity'; track: number; step: number; y: number; start: number };

const cellOf = (el: Element | null) => {
  const c = el?.closest<HTMLElement>('[data-step]');
  if (!c) return null;
  const step = Number(c.dataset.step);
  const track = c.dataset.track === undefined ? -1 : Number(c.dataset.track);
  return Number.isFinite(step) ? { track, step } : null;
};

/**
 * Tracks × steps editor. Click toggles a step, click-drag paints along the drag, shift-drag (or the Up/Down keys)
 * sets the velocity (bar height). The accent row toggles accents. Bars/beats are shaded; the playing column glows.
 */
export const StepGrid = memo(function StepGrid({ pattern, playStep, onEdit }: Props) {
  const gesture = useRef<Gesture | null>(null);
  const n = numSteps(pattern);

  const onPointerDown = (e: PointerEvent<HTMLDivElement>) => {
    const c = cellOf(e.target as Element);
    if (!c || e.button !== 0) return;
    e.preventDefault();
    if (c.track < 0) {
      onEdit((p) => toggleAccent(p, c.step));
      return;
    }
    const cur = pattern.tracks[c.track]?.steps[c.step] ?? 0;
    (e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId);
    if (e.shiftKey) {
      gesture.current = { kind: 'velocity', track: c.track, step: c.step, y: e.clientY, start: cur };
      if (cur === 0) onEdit((p) => setStepVelocity(p, c.track, c.step, DEFAULT_VELOCITY));
      return;
    }
    const value = cur > 0 ? 0 : DEFAULT_VELOCITY;
    gesture.current = { kind: 'paint', value, done: new Set([`${c.track}:${c.step}`]) };
    onEdit((p) => setStepVelocity(p, c.track, c.step, value));
  };

  const onPointerMove = (e: PointerEvent<HTMLDivElement>) => {
    const g = gesture.current;
    if (!g) return;
    if (g.kind === 'velocity') {
      const v = velocityFromDrag(g.start, e.clientY - g.y);
      onEdit((p) => setStepVelocity(p, g.track, g.step, v));
      return;
    }
    const c = cellOf(document.elementFromPoint(e.clientX, e.clientY));
    if (!c || c.track < 0) return;
    const key = `${c.track}:${c.step}`;
    if (g.done.has(key)) return;
    g.done.add(key);
    onEdit((p) => setStepVelocity(p, c.track, c.step, g.value));
  };

  const end = () => {
    gesture.current = null;
  };

  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    const c = cellOf(e.target as Element);
    if (!c) return;
    if (c.track < 0) {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        onEdit((p) => toggleAccent(p, c.step));
      }
      return;
    }
    const cur = pattern.tracks[c.track]?.steps[c.step] ?? 0;
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      onEdit((p) => setStepVelocity(p, c.track, c.step, cur > 0 ? 0 : DEFAULT_VELOCITY));
    } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      const v = Math.max(1, Math.min(127, (cur || DEFAULT_VELOCITY) + (e.key === 'ArrowUp' ? 10 : -10)));
      onEdit((p) => setStepVelocity(p, c.track, c.step, v));
    }
  };

  const stepClass = (s: number) =>
    `${isBarStart(pattern, s) ? 'bar-start ' : isBeatStart(pattern, s) ? 'beat-start ' : ''}${
      Math.floor(s / pattern.steps_per_beat) % 2 ? 'beat-odd ' : ''
    }${s === playStep ? 'now ' : ''}`;

  return (
    <div className="step-grid" style={{ ['--steps' as string]: n }}>
      <div
        className="sg-body"
        role="grid"
        aria-label="Drum pattern steps"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={end}
        onPointerCancel={end}
        onLostPointerCapture={end}
        onKeyDown={onKeyDown}
      >
        <div className="sg-row sg-ruler" role="row">
          <div className="sg-head" />
          {Array.from({ length: n }, (_, s) => (
            <div key={s} className={`sg-num ${stepClass(s)}`} aria-hidden>
              {isBeatStart(pattern, s) ? s / pattern.steps_per_beat + 1 : ''}
            </div>
          ))}
        </div>
        {pattern.tracks.map((t, k) => (
          <div key={k} className={`sg-row ${t.mute ? 'muted' : ''}`} role="row">
            <div className="sg-head" role="rowheader">
              <button
                type="button"
                className={`sg-mute ${t.mute ? 'on' : ''}`}
                aria-pressed={t.mute}
                title={t.mute ? 'Unmute track' : 'Mute track'}
                onClick={() => onEdit((p) => toggleMute(p, k))}
              >
                M
              </button>
              <select
                className="sg-note"
                value={t.note}
                aria-label={`Track ${k + 1} sound`}
                onChange={(e) => onEdit((p) => setTrackNote(p, k, Number(e.target.value)))}
              >
                {!DRUM_NOTES.some((d) => d.note === t.note) && <option value={t.note}>{t.name}</option>}
                {DRUM_NOTES.map((d) => (
                  <option key={d.note} value={d.note}>
                    {d.note === t.note && t.name ? t.name : d.name}
                  </option>
                ))}
              </select>
              <button
                type="button"
                className="sg-del"
                title="Remove track"
                aria-label={`Remove ${t.name}`}
                onClick={() => onEdit((p) => removeTrack(p, k))}
              >
                ×
              </button>
            </div>
            {Array.from({ length: n }, (_, s) => {
              const v = t.steps[s] ?? 0;
              const acc = (pattern.accent[s] ?? 0) > 0;
              return (
                <div
                  key={s}
                  role="gridcell"
                  tabIndex={0}
                  aria-label={`${t.name} step ${s + 1}${v ? ` velocity ${v}` : ' off'}`}
                  aria-selected={v > 0}
                  data-track={k}
                  data-step={s}
                  className={`sg-cell ${stepClass(s)}${v ? 'on ' : ''}${v && acc ? 'acc' : ''}`}
                  style={v ? { ['--vel' as string]: v / 127 } : undefined}
                >
                  <i />
                </div>
              );
            })}
          </div>
        ))}
        <div className="sg-row sg-accent" role="row">
          <div className="sg-head sg-accent-label" role="rowheader">
            Accent
          </div>
          {Array.from({ length: n }, (_, s) => (
            <div
              key={s}
              role="gridcell"
              tabIndex={0}
              aria-label={`Accent step ${s + 1}${pattern.accent[s] ? ' on' : ' off'}`}
              aria-selected={(pattern.accent[s] ?? 0) > 0}
              data-step={s}
              className={`sg-cell acc-cell ${stepClass(s)}${pattern.accent[s] ? 'on' : ''}`}
            >
              <i />
            </div>
          ))}
        </div>
      </div>
      {pattern.tracks.length < MAX_TRACKS && (
        <button type="button" className="sg-add" onClick={() => onEdit((p) => addTrack(p, 42))}>
          + Track
        </button>
      )}
    </div>
  );
});
