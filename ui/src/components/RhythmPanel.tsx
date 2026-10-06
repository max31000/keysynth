import { useEffect, useMemo, useState } from 'react';
import { actions, useEngine } from '../store';
import { live, onFrame } from '../store/liveBus';
import type { ParamSpec, Pattern, PatternEntry } from '../protocol/types';
import { DENOMINATORS, MAX_BARS, clearSteps, hitCount, numSteps, setBars, setStepsPerBeat } from '../lib/stepGrid';
import { Knob } from './Knob';
import { ModulePanel } from './ModulePanel';
import { StepGrid } from './StepGrid';

const spec = (id: string, name: string, def: number, unit = ''): ParamSpec => ({
  id,
  name,
  group: '',
  unit,
  min: 0,
  max: 1,
  def,
  scale: 'linear',
  skewCentre: 0,
  flags: 0,
  choices: [],
});
const SWING = spec('swing', 'Swing', 0);
const ACCENT = spec('accent_amount', 'Accent', 0.5);
const DRUMS_VOL = spec('drums_volume', 'Drums', 0.8);
const CLICK_VOL = spec('metronome_volume', 'Click', 0.5);

const editPattern = (fn: (p: Pattern) => Pattern) => actions().editPattern(fn);

const OPEN_KEY = 'ks.rhythm.open';
const TAB_KEY = 'ks.rhythm.tab';
const readPref = (k: string, d: string) => {
  try {
    return localStorage.getItem(k) ?? d;
  } catch {
    return d;
  }
};
const writePref = (k: string, v: string) => {
  try {
    localStorage.setItem(k, v);
  } catch {
    /* private mode */
  }
};

/** Playing step from telemetry (re-renders only when the step changes). */
function usePlayStep(): number {
  const [step, setStep] = useState(-1);
  useEffect(
    () =>
      onFrame(() => {
        const t = live.telemetry?.transport;
        const s = t && t.playing ? t.step : -1;
        setStep((prev) => (prev === s ? prev : s));
      }),
    [],
  );
  return step;
}

function PatternList({ patterns, current, onPick }: { patterns: PatternEntry[]; current: string; onPick: (p: string) => void }) {
  const groups: [string, PatternEntry[]][] = [
    ['Factory', patterns.filter((p) => p.factory)],
    ['User', patterns.filter((p) => !p.factory)],
  ];
  return (
    <div className="pattern-list" role="listbox" aria-label="Patterns">
      {groups.map(([g, list]) =>
        list.length ? (
          <div key={g} className="pl-group">
            <div className="pl-label">{g}</div>
            {list.map((p) => (
              <button
                type="button"
                role="option"
                aria-selected={p.path === current}
                key={p.path}
                className={`pl-item ${p.path === current ? 'on' : ''}`}
                onClick={() => onPick(p.path)}
                title={p.path}
              >
                <span className="pl-name">{p.name}</span>
                <span className="pl-meta mono">
                  {p.time_sig[0]}/{p.time_sig[1]}
                  {p.tempo ? ` · ${Math.round(p.tempo)}` : ''}
                </span>
              </button>
            ))}
          </div>
        ) : null,
      )}
    </div>
  );
}

function SaveRow({ name }: { name: string }) {
  const [value, setValue] = useState(name);
  useEffect(() => setValue(name), [name]);
  const save = async (overwrite = false) => {
    const n = value.trim();
    if (!n) return;
    const r = await actions().savePattern(n, overwrite);
    if (r === null && !overwrite && actions().lastError?.includes('already exists') && confirm(`Overwrite pattern "${n}"?`)) {
      actions().clearError();
      await actions().savePattern(n, true);
    }
  };
  return (
    <form
      className="pl-save"
      onSubmit={(e) => {
        e.preventDefault();
        void save();
      }}
    >
      <input className="input" value={value} onChange={(e) => setValue(e.target.value)} aria-label="Pattern name" maxLength={64} />
      <button type="submit" className="btn">
        Save
      </button>
    </form>
  );
}

function Meter({ pattern }: { pattern: Pattern }) {
  const [num, den] = pattern.time_sig;
  const ts = (n: number, d: number) => actions().setTransport({ time_sig: [n, d] });
  return (
    <div className="rh-meter">
      <label className="rh-field">
        <span className="ctl-label">Meter</span>
        <span className="rh-ts">
          <select className="select" value={num} aria-label="Beats per bar" onChange={(e) => ts(Number(e.target.value), den)}>
            {Array.from({ length: 16 }, (_, i) => i + 1).map((v) => (
              <option key={v} value={v}>
                {v}
              </option>
            ))}
          </select>
          <span className="rh-slash">/</span>
          <select className="select" value={den} aria-label="Beat unit" onChange={(e) => ts(num, Number(e.target.value))}>
            {DENOMINATORS.map((v) => (
              <option key={v} value={v}>
                {v}
              </option>
            ))}
          </select>
        </span>
      </label>
      <label className="rh-field">
        <span className="ctl-label">Bars</span>
        <select
          className="select"
          value={pattern.bars}
          aria-label="Bars"
          onChange={(e) => actions().editPattern((p) => setBars(p, Number(e.target.value)))}
        >
          {Array.from({ length: MAX_BARS }, (_, i) => i + 1).map((v) => (
            <option key={v} value={v}>
              {v}
            </option>
          ))}
        </select>
      </label>
      <label className="rh-field">
        <span className="ctl-label">Steps/beat</span>
        <select
          className="select"
          value={pattern.steps_per_beat}
          aria-label="Steps per beat"
          onChange={(e) => actions().editPattern((p) => setStepsPerBeat(p, Number(e.target.value)))}
        >
          {[1, 2, 3, 4, 6, 8].map((v) => (
            <option key={v} value={v}>
              {v}
            </option>
          ))}
        </select>
      </label>
    </div>
  );
}

/** Collapsible drum machine: pattern browser, transport, meter/swing, step grid, kit params. */
export function RhythmPanel() {
  const [open, setOpen] = useState(() => readPref(OPEN_KEY, '1') === '1');
  const [tab, setTab] = useState<'steps' | 'kit'>(() => (readPref(TAB_KEY, 'steps') === 'kit' ? 'kit' : 'steps'));
  const t = useEngine((s) => s.transport);
  const snap = useEngine((s) => s.pattern);
  const patterns = useEngine((s) => s.patterns);
  const kitNode = useEngine((s) => s.patch?.rhythm?.node ?? null);
  const kitSpec = useEngine((s) => s.catalogMap.drums?.params.find((p) => p.id === 'kit'));
  const kitValue = useEngine((s) => s.patch?.rhythm?.kit?.kit ?? 0);
  const playStep = usePlayStep();
  const pattern = snap?.pattern;
  const steps = pattern ? numSteps(pattern) : 0;
  const hits = useMemo(() => (pattern ? hitCount(pattern) : 0), [pattern]);

  const toggle = () => {
    setOpen((o) => {
      writePref(OPEN_KEY, o ? '0' : '1');
      return !o;
    });
  };
  const pickTab = (v: 'steps' | 'kit') => {
    setTab(v);
    writePref(TAB_KEY, v);
  };
  const drumsOn = t.drums !== false;

  return (
    <section className={`rhythm panel ${open ? 'open' : ''}`} aria-label="Rhythm">
      <header className="rhythm-head">
        <button type="button" className="rh-toggle" aria-expanded={open} onClick={toggle} title={open ? 'Collapse' : 'Expand'}>
          <span className="chev" aria-hidden>
            ▸
          </span>
          Rhythm
        </button>
        <button
          type="button"
          className={`transport-btn ${t.playing ? 'playing' : ''}`}
          onClick={() => actions().setTransport({ playing: !t.playing })}
          aria-label={t.playing ? 'Stop' : 'Play'}
          title={t.playing ? 'Stop' : 'Play'}
        >
          {t.playing ? (
            <svg viewBox="0 0 16 16" width="11" height="11">
              <rect x="3" y="3" width="10" height="10" fill="currentColor" />
            </svg>
          ) : (
            <svg viewBox="0 0 16 16" width="11" height="11">
              <path d="M4 2.5 L13.5 8 L4 13.5 Z" fill="currentColor" />
            </svg>
          )}
        </button>
        <span className="rh-name" title={snap?.path || 'unsaved pattern'}>
          {pattern?.name ?? '—'}
          {snap?.edited && <span className="rh-edited" title="Edited (not saved)">●</span>}
        </span>
        <span className="chip mono">
          {pattern ? `${pattern.time_sig[0]}/${pattern.time_sig[1]}` : '-/-'}
        </span>
        <span className="chip mono">{Math.round(t.tempo * 10) / 10} BPM</span>
        <div className="rh-steps" aria-hidden>
          {pattern &&
            Array.from({ length: Math.min(steps, 64) }, (_, s) => (
              <i key={s} className={`${s === playStep ? 'now' : ''} ${s % pattern.steps_per_beat === 0 ? 'beat' : ''}`} />
            ))}
        </div>
        <div className="spacer" />
        <button
          type="button"
          className={`led-btn ${t.metronome ? 'on' : ''}`}
          aria-pressed={t.metronome}
          onClick={() => actions().setTransport({ metronome: !t.metronome })}
        >
          <span className="led" />
          Metro
        </button>
        <Knob
          spec={CLICK_VOL}
          value={t.metronome_volume}
          size={26}
          bare
          className="knob-mini"
          onChange={(v) => actions().setTransport({ metronome_volume: v })}
        />
        <button
          type="button"
          className={`led-btn ${t.count_in ? 'on' : ''}`}
          aria-pressed={!!t.count_in}
          onClick={() => actions().setTransport({ count_in: !t.count_in })}
          title="One bar of clicks before the pattern starts"
        >
          <span className="led" />
          Count-in
        </button>
        <div className="rh-sep" />
        <button
          type="button"
          className={`led-btn ${drumsOn ? 'on' : ''}`}
          aria-pressed={drumsOn}
          onClick={() => actions().setTransport({ drums: !drumsOn })}
          title="Drum sequencer output"
        >
          <span className="led" />
          Drums
        </button>
        {kitSpec && kitNode !== null && (
          <label className="rh-kit">
            <span className="ctl-label">Kit</span>
            <select
              className="select"
              value={Math.round(kitValue)}
              aria-label="Drum kit"
              onChange={(e) => actions().setParam(kitNode, 'kit', Number(e.target.value))}
            >
              {kitSpec.choices.map((c, i) => (
                <option key={c} value={i}>
                  {c}
                </option>
              ))}
            </select>
          </label>
        )}
        {open && (
          <div className="segmented rh-tabs" role="tablist" aria-label="Rhythm view">
            <button type="button" role="tab" aria-selected={tab === 'steps'} className={tab === 'steps' ? 'on' : ''} onClick={() => pickTab('steps')}>
              Steps
            </button>
            <button type="button" role="tab" aria-selected={tab === 'kit'} className={tab === 'kit' ? 'on' : ''} onClick={() => pickTab('kit')}>
              Kit
            </button>
          </div>
        )}
      </header>
      {open && (
        <div className="rhythm-body">
          <aside className="rh-browser">
            <PatternList patterns={patterns} current={snap?.path ?? ''} onPick={(p) => actions().loadPattern(p)} />
            <button type="button" className="btn pl-new" onClick={() => actions().loadPattern('')}>
              New empty
            </button>
            {pattern && <SaveRow name={pattern.name} />}
          </aside>
          <div className="rh-main">
            <div className="rh-controls">
              {pattern && <Meter pattern={pattern} />}
              <Knob spec={SWING} value={t.swing ?? 0} size={34} onChange={(v) => actions().setTransport({ swing: v })} />
              <Knob
                spec={ACCENT}
                value={pattern?.accent_amount ?? 0.5}
                size={34}
                onChange={(v) => actions().editPattern((p) => ({ ...p, accent_amount: v }))}
              />
              <Knob spec={DRUMS_VOL} value={t.drums_volume ?? 0.8} size={34} onChange={(v) => actions().setTransport({ drums_volume: v })} />
              <div className="spacer" />
              <span className="muted mono rh-hits">
                {hits} hits · {steps} steps
              </span>
              <button type="button" className="btn" onClick={() => actions().editPattern(clearSteps)} disabled={!hits}>
                Clear
              </button>
            </div>
            <div className="rh-view">
              {tab === 'steps' ? (
                pattern ? (
                  <StepGrid pattern={pattern} playStep={playStep} onEdit={editPattern} />
                ) : (
                  <div className="empty-state">No pattern loaded.</div>
                )
              ) : (
                <ModulePanel node={kitNode} />
              )}
            </div>
          </div>
        </div>
      )}
    </section>
  );
}
