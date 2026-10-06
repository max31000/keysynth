import { useEffect, useRef, useState } from 'react';
import { actions, useEngine } from '../store';
import { live, onFrame } from '../store/liveBus';
import type { ParamSpec } from '../protocol/types';
import { LATENCY_WARN_MS, bufferMs, latencyHint, latencyTooHigh } from '../lib/latency';
import { Knob } from './Knob';

const METRO_SPEC: ParamSpec = {
  id: 'metronome_volume',
  name: 'Click',
  group: '',
  unit: '',
  min: 0,
  max: 1,
  def: 0.5,
  scale: 'linear',
  skewCentre: 0,
  flags: 0,
  choices: [],
};

function ConnectionPill() {
  const c = useEngine((s) => s.connection);
  const label =
    c.status === 'open'
      ? 'Engine'
      : c.status === 'reconnecting'
        ? `Retry ${c.attempt}`
        : c.status === 'connecting'
          ? 'Connecting'
          : 'Offline';
  return (
    <div className={`conn conn-${c.status}`} title={`${c.url} — ${c.status}`}>
      <span className="led" />
      <span>{label}</span>
    </div>
  );
}

function CpuMeter() {
  const fill = useRef<HTMLDivElement>(null);
  const txt = useRef<HTMLSpanElement>(null);
  useEffect(() => {
    let smooth = 0;
    let shown = -1;
    return onFrame(() => {
      const cpu = live.telemetry?.cpu ?? 0;
      smooth += (cpu - smooth) * 0.15;
      if (fill.current) {
        fill.current.style.transform = `scaleX(${Math.min(1, smooth).toFixed(3)})`;
        fill.current.dataset.level = smooth > 0.8 ? 'hot' : smooth > 0.55 ? 'warm' : 'ok';
      }
      const pct = Math.round(smooth * 100);
      if (txt.current && pct !== shown) {
        shown = pct;
        txt.current.textContent = `${pct}%`;
      }
    });
  }, []);
  return (
    <div className="stat cpu" title="DSP load (callback time / buffer duration)">
      <span className="stat-k">CPU</span>
      <div className="cpu-bar">
        <div ref={fill} className="cpu-fill" />
      </div>
      <span ref={txt} className="stat-v mono">
        0%
      </span>
    </div>
  );
}

function TempoField() {
  const tempo = useEngine((s) => s.transport.tempo);
  const [edit, setEdit] = useState<string | null>(null);
  const drag = useRef<{ y: number; t: number } | null>(null);
  const commit = (v: number) => {
    if (Number.isFinite(v)) actions().setTransport({ tempo: Math.round(Math.min(300, Math.max(20, v)) * 10) / 10 });
  };
  if (edit !== null) {
    return (
      <input
        className="tempo-input mono"
        autoFocus
        value={edit}
        onChange={(e) => setEdit(e.target.value)}
        onBlur={() => {
          commit(parseFloat(edit));
          setEdit(null);
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
          if (e.key === 'Escape') setEdit(null);
        }}
      />
    );
  }
  return (
    <div
      className="tempo mono"
      role="spinbutton"
      tabIndex={0}
      aria-label="Tempo"
      aria-valuenow={tempo}
      title="Tempo — drag, wheel, or double-click to type"
      onPointerDown={(e) => {
        (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
        drag.current = { y: e.clientY, t: tempo };
      }}
      onPointerMove={(e) => {
        const d = drag.current;
        if (!d) return;
        const step = e.shiftKey ? 0.1 : 0.5;
        commit(d.t + Math.round((d.y - e.clientY) * step * 10) / 10);
      }}
      onPointerUp={() => (drag.current = null)}
      onDoubleClick={() => setEdit(String(tempo))}
      onWheel={(e) => commit(tempo + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 0.1 : 1))}
      onKeyDown={(e) => {
        if (e.key === 'ArrowUp') commit(tempo + 1);
        if (e.key === 'ArrowDown') commit(tempo - 1);
      }}
    >
      {tempo.toFixed(tempo % 1 ? 1 : 0)}
      <span className="unit">BPM</span>
    </div>
  );
}

export function TopBar({ onAudioSettings }: { onAudioSettings: () => void }) {
  const audio = useEngine((s) => s.audio);
  const stats = useEngine((s) => s.stats);
  const t = useEngine((s) => s.transport);
  const open = useEngine((s) => s.connection.status === 'open');
  const slow = latencyTooHigh(audio);

  return (
    <header className="topbar">
      <div className="brand">
        <span className="brand-mark" aria-hidden>
          <i />
          <i />
          <i />
        </span>
        <span className="brand-name">keysynth</span>
      </div>
      <ConnectionPill />

      <button type="button" className="audio-info" onClick={onAudioSettings} title="Audio settings">
        {audio ? (
          <>
            <span className={`led ${audio.running ? 'on' : 'off'}`} />
            <span className="audio-dev">{audio.name}</span>
            <span className="chip mono sr">{(audio.sampleRate / 1000).toFixed(audio.sampleRate % 1000 ? 1 : 0)}k</span>
            <span className="chip mono" title={`buffer ${bufferMs(audio).toFixed(2)} ms`}>
              {audio.bufferSize} smp
            </span>
            <span
              className={`chip mono ${slow ? 'warn' : 'accent'}`}
              title={`output latency (device-reported, incl. buffer ${bufferMs(audio).toFixed(2)} ms)`}
            >
              out {audio.outputLatencyMs.toFixed(1)} ms
            </span>
          </>
        ) : (
          <span className="muted">No audio device</span>
        )}
      </button>
      {slow && audio && (
        <div className="latency-badge" role="status">
          <span className="latency-badge-text" title={`Output latency above ${LATENCY_WARN_MS} ms: ${latencyHint(audio)}`}>
            ⚠ High latency
          </span>
          {audio.hasControlPanel && (
            <button type="button" className="btn small" onClick={() => void actions().openAudioPanel()} title={latencyHint(audio)} disabled={!!audio.panelOpen}>
              Open ASIO panel
            </button>
          )}
        </div>
      )}

      <CpuMeter />
      <div className="stat" title="Buffer under/overruns since start">
        <span className="stat-k">XRUN</span>
        <span className={`stat-v mono ${stats.xruns ? 'warn' : ''}`}>{stats.xruns}</span>
      </div>
      <div className="stat voices" title="Active voices">
        <span className="stat-k">VOX</span>
        <span className="stat-v mono">{stats.voices}</span>
      </div>

      <div className="spacer" />

      <div className="transport">
        <TempoField />
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
          spec={METRO_SPEC}
          value={t.metronome_volume}
          onChange={(v) => actions().setTransport({ metronome_volume: v })}
          size={28}
          bare
          className="knob-mini"
        />
        <button
          type="button"
          className={`transport-btn ${t.playing ? 'playing' : ''}`}
          onClick={() => actions().setTransport({ playing: !t.playing })}
          aria-label={t.playing ? 'Stop' : 'Play'}
          title={t.playing ? 'Stop' : 'Play'}
        >
          {t.playing ? (
            <svg viewBox="0 0 16 16" width="12" height="12">
              <rect x="3" y="3" width="10" height="10" fill="currentColor" />
            </svg>
          ) : (
            <svg viewBox="0 0 16 16" width="12" height="12">
              <path d="M4 2.5 L13.5 8 L4 13.5 Z" fill="currentColor" />
            </svg>
          )}
        </button>
      </div>

      <button type="button" className="panic" disabled={!open} onClick={() => actions().panic()} title="All notes off, reset tails">
        Panic
      </button>
    </header>
  );
}
