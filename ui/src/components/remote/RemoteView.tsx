// «Пульт»: touch view for playing (iPad at the piano; docs/specs/2026-10-12-remote.md §6): large preset tiles
// (favourites / recent / all + search), current preset, master volume, drums start/stop + tempo, panic. The full
// editor stays one tap away. Keeps the screen on while open (Wake Lock). Song mode: pick a song, big section
// buttons, prev / next, the current section's notes (PROTOCOL.md "Songs"; allowed for remote sessions).

import { useEffect, useMemo, useState } from 'react';
import { actions, songActions, useEngine, useRemote, useSongs } from '../../store';
import { currentSection } from '../../store/songStore';
import { SongSections } from '../songs/SongSections';
import { MASTER_NODE } from '../../protocol/types';
import { visiblePresets, type PresetTab as Tab } from '../../lib/presetTiles';
import { useFavourites } from '../../lib/favourites';
import { useRecent } from '../../lib/recent';
import { StereoMeter, masterSource } from '../Meter';
import { AccountButton } from '../cloud/AccountButton';
import { RelayPill } from './RelayStatus';
import { ConnectionPill } from '../TopBar';

const TABS: { id: Tab; label: string }[] = [
  { id: 'fav', label: 'Избранное' },
  { id: 'recent', label: 'Недавние' },
  { id: 'all', label: 'Все' },
];

type PultMode = 'presets' | 'songs';
const MODE_KEY = 'keysynth.pult.mode';
const readMode = (): PultMode => {
  try {
    return localStorage.getItem(MODE_KEY) === 'songs' ? 'songs' : 'presets';
  } catch {
    return 'presets';
  }
};

const VOL_MIN = -60;
const VOL_MAX = 6;

interface WakeLockSentinelLike {
  release(): Promise<void>;
}

/** Screen Wake Lock while mounted; re-acquired when the tab becomes visible again (the browser drops it). */
function useWakeLock() {
  useEffect(() => {
    const wl = (navigator as { wakeLock?: { request(t: 'screen'): Promise<WakeLockSentinelLike> } }).wakeLock;
    if (!wl) return;
    let sentinel: WakeLockSentinelLike | null = null;
    let alive = true;
    const acquire = () => {
      if (document.visibilityState !== 'visible') return;
      wl.request('screen').then(
        (s) => {
          if (alive) sentinel = s;
          else void s.release();
        },
        () => undefined, // not allowed (battery saver, iframe policy)
      );
    };
    acquire();
    document.addEventListener('visibilitychange', acquire);
    return () => {
      alive = false;
      document.removeEventListener('visibilitychange', acquire);
      void sentinel?.release().catch(() => undefined);
    };
  }, []);
}

function PresetTiles() {
  const presets = useEngine((s) => s.presets);
  const current = useEngine((s) => s.presetPath);
  const [favs, toggleFav] = useFavourites();
  const recent = useRecent();
  const [tab, setTab] = useState<Tab>(() => (favs.size ? 'fav' : 'all'));
  const [query, setQuery] = useState('');
  const [loading, setLoading] = useState<string | null>(null);
  const list = useMemo(() => visiblePresets(presets, tab, query, favs, recent), [presets, tab, query, favs, recent]);

  const load = async (path: string) => {
    setLoading(path);
    await actions().loadPreset(path);
    setLoading(null);
  };

  return (
    <section className="pult-presets" aria-label="Пресеты">
      <div className="pult-tabs">
        <div className="pult-seg" role="tablist">
          {TABS.map((t) => (
            <button
              key={t.id}
              type="button"
              role="tab"
              aria-selected={!query && tab === t.id}
              className={!query && tab === t.id ? 'on' : ''}
              onClick={() => {
                setTab(t.id);
                setQuery('');
              }}
            >
              {t.label}
            </button>
          ))}
        </div>
        <input
          className="pult-search"
          type="search"
          placeholder="Поиск"
          aria-label="Поиск пресета"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </div>
      <div className="pult-grid">
        {list.map((p) => (
          <div key={p.path} className={`pult-tile${p.path === current ? ' current' : ''}${loading === p.path ? ' loading' : ''}`}>
            <button type="button" className="pult-tile-main" onClick={() => void load(p.path)} aria-pressed={p.path === current}>
              <span className="pult-tile-name">{p.name}</span>
              <span className="pult-tile-cat">{p.category}</span>
            </button>
            <button
              type="button"
              className={`pult-star${favs.has(p.path) ? ' on' : ''}`}
              aria-label={favs.has(p.path) ? 'Убрать из избранного' : 'В избранное'}
              aria-pressed={favs.has(p.path)}
              onClick={() => toggleFav(p.path)}
            >
              ★
            </button>
          </div>
        ))}
        {list.length === 0 && (
          <p className="pult-empty muted">
            {query ? 'Ничего не найдено.' : tab === 'fav' ? 'Нет избранных — отметьте ★.' : tab === 'recent' ? 'Пока пусто.' : 'Нет пресетов.'}
          </p>
        )}
      </div>
    </section>
  );
}

function SongPult() {
  const songs = useSongs((s) => s.songs);
  const loaded = useSongs((s) => s.loaded);
  const state = useSongs((s) => s.state);
  const error = useSongs((s) => s.error);
  const open = useEngine((s) => s.connection.status === 'open');
  const [picking, setPicking] = useState(false);
  const song = state?.song ?? null;
  const sec = currentSection(state);

  if (!song || picking) {
    return (
      <section className="pult-presets pult-songs" aria-label="Песни">
        <div className="pult-tabs">
          <span className="panel-label">Песни</span>
          <span className="spacer" />
          {song && (
            <button type="button" className="btn" onClick={() => setPicking(false)}>
              Назад
            </button>
          )}
        </div>
        <div className="pult-grid">
          {songs.map((s) => (
            <div key={s.path} className={`pult-tile${s.path === state?.path ? ' current' : ''}`}>
              <button
                type="button"
                className="pult-tile-main"
                aria-pressed={s.path === state?.path}
                onClick={() => {
                  setPicking(false);
                  void songActions().select(s.path);
                }}
              >
                <span className="pult-tile-name">{s.title}</span>
                <span className="pult-tile-cat">
                  {s.factory ? '' : 'Моя · '}
                  {s.artist || `${s.sections} секц.`}
                </span>
              </button>
            </div>
          ))}
          {songs.length === 0 && <p className="pult-empty muted">{loaded ? 'Нет песен.' : 'Загрузка…'}</p>}
        </div>
        {error && <p className="pult-song-error" role="alert">{error}</p>}
      </section>
    );
  }
  return (
    <section className="pult-presets pult-songs" aria-label="Песня">
      <div className="pult-tabs pult-song-head">
        <div className="pult-song-title">
          <span className="pult-tile-name">{song.title}</span>
          {song.artist && <span className="pult-tile-cat">{song.artist}</span>}
        </div>
        <button type="button" className="btn" onClick={() => setPicking(true)}>
          Список
        </button>
        <button type="button" className="btn" onClick={() => void songActions().close()}>
          Закрыть
        </button>
      </div>
      <SongSections
        big
        disabled={!open}
        sections={song.sections}
        current={state!.section}
        onSection={(i) => void songActions().section(i)}
        onPrev={() => void songActions().prev()}
        onNext={() => void songActions().next()}
      />
      {(sec?.notes || song.notes) && (
        <div className="pult-song-notes">
          {sec?.notes && <p className="pult-song-notes-sec">{sec.notes}</p>}
          {song.notes && <p className="muted">{song.notes}</p>}
        </div>
      )}
      {error && <p className="pult-song-error" role="alert">{error}</p>}
    </section>
  );
}

function MasterVolume() {
  const vol = useEngine((s) => s.patch?.master.volume_db ?? -3);
  return (
    <div className="pult-card pult-volume">
      <div className="pult-card-head">
        <span className="panel-label">Громкость</span>
        <span className="mono pult-value">{vol <= VOL_MIN ? '−∞' : `${vol > 0 ? '+' : ''}${vol.toFixed(1)}`} dB</span>
      </div>
      <input
        className="pult-range"
        type="range"
        min={VOL_MIN}
        max={VOL_MAX}
        step={0.5}
        value={vol}
        aria-label="Громкость"
        onChange={(e) => actions().setParam(MASTER_NODE, 'volume_db', Number(e.target.value))}
      />
      <StereoMeter source={masterSource} orientation="horizontal" className="pult-meter" />
    </div>
  );
}

const clampTempo = (v: number) => Math.round(Math.min(300, Math.max(20, v)));

function Drums() {
  const t = useEngine((s) => s.transport);
  const patternName = useEngine((s) => s.pattern?.pattern.name ?? null);
  const setTempo = (v: number) => actions().setTransport({ tempo: clampTempo(v) });
  return (
    <div className="pult-card pult-drums">
      <div className="pult-card-head">
        <span className="panel-label">Ритм</span>
        {patternName && <span className="pult-pattern">{patternName}</span>}
      </div>
      <div className="pult-drums-row">
        <button
          type="button"
          className={`pult-play${t.playing ? ' playing' : ''}`}
          aria-pressed={t.playing}
          onClick={() => actions().setTransport({ playing: !t.playing })}
        >
          {t.playing ? (
            <svg viewBox="0 0 16 16" width="22" height="22" aria-hidden>
              <rect x="3" y="3" width="10" height="10" fill="currentColor" />
            </svg>
          ) : (
            <svg viewBox="0 0 16 16" width="22" height="22" aria-hidden>
              <path d="M4 2.5 L13.5 8 L4 13.5 Z" fill="currentColor" />
            </svg>
          )}
          <span>{t.playing ? 'Стоп' : 'Старт'}</span>
        </button>
        <div className="pult-tempo" role="group" aria-label="Темп">
          <button type="button" aria-label="Темп −5" onClick={() => setTempo(t.tempo - 5)}>
            −5
          </button>
          <button type="button" aria-label="Темп −1" onClick={() => setTempo(t.tempo - 1)}>
            −
          </button>
          <span className="mono pult-bpm">
            {Math.round(t.tempo)}
            <small>BPM</small>
          </span>
          <button type="button" aria-label="Темп +1" onClick={() => setTempo(t.tempo + 1)}>
            +
          </button>
          <button type="button" aria-label="Темп +5" onClick={() => setTempo(t.tempo + 5)}>
            +5
          </button>
        </div>
      </div>
    </div>
  );
}

export function RemoteView({ onEditor }: { onEditor: () => void }) {
  useWakeLock();
  const presets = useEngine((s) => s.presets);
  const path = useEngine((s) => s.presetPath);
  const dirty = useEngine((s) => s.dirty);
  const open = useEngine((s) => s.connection.status === 'open');
  const relay = useRemote((s) => s.route === 'relay');
  const patchName = useEngine((s) => s.patch?.meta.name ?? null);
  const name = presets.find((p) => p.path === path)?.name ?? patchName;
  const [mode, setModeState] = useState<PultMode>(readMode);
  const setMode = (m: PultMode) => {
    setModeState(m);
    try {
      localStorage.setItem(MODE_KEY, m);
    } catch {
      /* private mode */
    }
  };

  return (
    <div className="pult">
      <header className="pult-top">
        <div className="brand">
          <span className="brand-mark" aria-hidden>
            <i />
            <i />
            <i />
          </span>
        </div>
        <div className="pult-current" aria-live="polite">
          <span className="panel-label">Пресет</span>
          <span className="pult-current-name">
            {name ?? '—'}
            {dirty && <i className="pult-dirty" title="Изменён" />}
          </span>
        </div>
        <div className="pult-seg pult-mode" role="tablist" aria-label="Режим">
          {(['presets', 'songs'] as const).map((m) => (
            <button key={m} type="button" role="tab" aria-selected={mode === m} className={mode === m ? 'on' : ''} onClick={() => setMode(m)}>
              {m === 'presets' ? 'Пресеты' : 'Песни'}
            </button>
          ))}
        </div>
        {relay ? <RelayPill /> : <ConnectionPill />}
        <button type="button" className="btn pult-editor" onClick={onEditor}>
          Редактор
        </button>
        <AccountButton />
      </header>
      {mode === 'songs' ? <SongPult /> : <PresetTiles />}
      <aside className="pult-side">
        <MasterVolume />
        <Drums />
        <button type="button" className="pult-panic" disabled={!open} onClick={() => actions().panic()}>
          Panic
        </button>
      </aside>
    </div>
  );
}
