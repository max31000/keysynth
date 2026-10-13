// Song editor (PRESETS.md "Songs"): metadata, per-song control keys, sections (preset / pattern / drums / tempo /
// notes). Edits a local draft; «Сохранить» sends `song_save` (a factory song is saved as a user copy).

import { useMemo, useState } from 'react';
import { songActions, useEngine } from '../../store';
import type { PatternEntry, PresetEntry, Song, SongSection } from '../../protocol/types';
import {
  DEFAULT_CONTROL_KEYS,
  SONG_LIMITS,
  TIME_SIG_DENOMINATORS,
  addSection,
  drumsMode,
  drumsValue,
  moveSection,
  removeSection,
  savePayload,
  updateSection,
  updateSong,
  validateSong,
  type DrumsMode,
  type SongOrigin,
} from '../../lib/songEdit';
import { songErrorText } from '../../store/songStore';
import { NoteSelect } from './NoteSelect';

const KEEP = '__keep';
const NUMERATORS = Array.from({ length: 16 }, (_, i) => i + 1);

const numOrUndef = (v: string): number | undefined => {
  if (v.trim() === '') return undefined;
  const n = Number(v);
  return Number.isFinite(n) ? n : undefined;
};

function PresetPicker({ value, presets, onChange }: { value: string | undefined; presets: PresetEntry[]; onChange(v: string | undefined): void }) {
  const factory = presets.filter((p) => p.factory);
  const user = presets.filter((p) => !p.factory);
  const known = value === undefined || presets.some((p) => p.path === value);
  return (
    <select className="select" aria-label="Пресет" value={value ?? ''} onChange={(e) => onChange(e.target.value || undefined)}>
      <option value="">— не менять</option>
      {!known && <option value={value}>{value}</option>}
      {user.length > 0 && (
        <optgroup label="Мои">
          {user.map((p) => (
            <option key={p.path} value={p.path}>
              {p.name}
            </option>
          ))}
        </optgroup>
      )}
      <optgroup label="Заводские">
        {factory.map((p) => (
          <option key={p.path} value={p.path}>
            {p.category ? `${p.category} · ${p.name}` : p.name}
          </option>
        ))}
      </optgroup>
    </select>
  );
}

function PatternPicker({ value, patterns, onChange }: { value: string | undefined; patterns: PatternEntry[]; onChange(v: string | undefined): void }) {
  const known = value === undefined || value === '' || patterns.some((p) => p.path === value);
  return (
    <select
      className="select"
      aria-label="Паттерн"
      value={value === undefined ? KEEP : value}
      onChange={(e) => onChange(e.target.value === KEEP ? undefined : e.target.value)}
    >
      <option value={KEEP}>— не менять</option>
      <option value="">Пустой</option>
      {!known && <option value={value}>{value}</option>}
      {patterns.map((p) => (
        <option key={p.path} value={p.path}>
          {p.name}
          {p.factory ? '' : ' (мой)'}
        </option>
      ))}
    </select>
  );
}

function SectionRow({
  section,
  index,
  count,
  presets,
  patterns,
  onChange,
  onMove,
  onRemove,
}: {
  section: SongSection;
  index: number;
  count: number;
  presets: PresetEntry[];
  patterns: PatternEntry[];
  onChange(patch: Partial<SongSection>): void;
  onMove(to: number): void;
  onRemove(): void;
}) {
  return (
    <li className="song-ed-section">
      <div className="song-ed-section-head">
        <span className="mono song-ed-num">{index + 1}</span>
        <input
          className="input song-ed-name"
          aria-label="Название секции"
          value={section.name}
          maxLength={SONG_LIMITS.sectionName}
          onChange={(e) => onChange({ name: e.target.value })}
        />
        <button type="button" className="icon-btn" aria-label="Выше" title="Выше" disabled={index === 0} onClick={() => onMove(index - 1)}>
          ↑
        </button>
        <button type="button" className="icon-btn" aria-label="Ниже" title="Ниже" disabled={index === count - 1} onClick={() => onMove(index + 1)}>
          ↓
        </button>
        <button type="button" className="icon-btn" aria-label="Удалить секцию" title="Удалить секцию" disabled={count <= 1} onClick={onRemove}>
          ×
        </button>
      </div>
      <div className="song-ed-grid">
        <label className="field">
          <span>Пресет</span>
          <PresetPicker value={section.preset} presets={presets} onChange={(preset) => onChange({ preset })} />
        </label>
        <label className="field">
          <span>Паттерн</span>
          <PatternPicker value={section.pattern} patterns={patterns} onChange={(pattern) => onChange({ pattern })} />
        </label>
        <label className="field">
          <span>Ударные</span>
          <select
            className="select"
            value={drumsMode(section)}
            onChange={(e) => onChange({ drums: drumsValue(e.target.value as DrumsMode) })}
          >
            <option value="keep">не менять</option>
            <option value="start">старт</option>
            <option value="stop">стоп</option>
          </select>
        </label>
        <label className="field song-ed-tempo">
          <span>Темп</span>
          <input
            className="input mono"
            type="number"
            min={SONG_LIMITS.tempoMin}
            max={SONG_LIMITS.tempoMax}
            placeholder="песни"
            value={section.tempo ?? ''}
            onChange={(e) => onChange({ tempo: numOrUndef(e.target.value) })}
          />
        </label>
      </div>
      <textarea
        className="input song-ed-notes"
        aria-label="Заметки секции"
        placeholder="Заметки"
        rows={1}
        maxLength={SONG_LIMITS.notes}
        value={section.notes ?? ''}
        onChange={(e) => onChange({ notes: e.target.value })}
      />
    </li>
  );
}

export function SongEditor({
  initial,
  origin,
  onDone,
  onCancel,
}: {
  initial: Song;
  origin: SongOrigin;
  onDone(path: string): void;
  onCancel(): void;
}) {
  const presets = useEngine((s) => s.presets);
  const patterns = useEngine((s) => s.patterns);
  const [song, setSong] = useState<Song>(initial);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [confirm, setConfirm] = useState(false);
  const [tried, setTried] = useState(false);
  const issues = useMemo(() => validateSong(song), [song]);

  const patchSong = (p: Partial<Song>) => setSong((s) => updateSong(s, p));
  const ck = song.control_keys;
  const ts = song.time_sig;

  const save = async (overwrite: boolean) => {
    setTried(true);
    if (issues.length) return;
    setBusy(true);
    setError(null);
    const payload = savePayload(song, origin);
    const r = await songActions().save(payload.song, overwrite || payload.overwrite);
    setBusy(false);
    if (r.ok) onDone(r.path);
    else if (r.code === 'exists') setConfirm(true);
    else setError(songErrorText(r.code, r.message));
  };

  return (
    <div className="song-ed">
      <div className="song-ed-top">
        <button type="button" className="btn small" onClick={onCancel}>
          ← Список
        </button>
        <span className="song-ed-title">{origin === 'new' ? 'Новая песня' : origin === 'factory' ? 'Заводская песня → копия' : 'Песня'}</span>
      </div>

      <div className="field-row">
        <label className="field">
          <span>Название</span>
          <input className="input" value={song.title} maxLength={SONG_LIMITS.title} onChange={(e) => patchSong({ title: e.target.value })} />
        </label>
        <label className="field">
          <span>Исполнитель</span>
          <input className="input" value={song.artist ?? ''} onChange={(e) => patchSong({ artist: e.target.value })} />
        </label>
      </div>
      <div className="field-row">
        <label className="field">
          <span>Темп</span>
          <input
            className="input mono"
            type="number"
            min={SONG_LIMITS.tempoMin}
            max={SONG_LIMITS.tempoMax}
            placeholder="—"
            value={song.tempo ?? ''}
            onChange={(e) => patchSong({ tempo: numOrUndef(e.target.value) })}
          />
        </label>
        <label className="field">
          <span>Размер</span>
          <span className="row gap">
            <select
              className="select mono"
              aria-label="Долей в такте"
              value={ts ? String(ts[0]) : ''}
              onChange={(e) => patchSong({ time_sig: e.target.value ? [Number(e.target.value), ts?.[1] ?? 4] : undefined })}
            >
              <option value="">—</option>
              {NUMERATORS.map((n) => (
                <option key={n} value={n}>
                  {n}
                </option>
              ))}
            </select>
            <span className="muted">/</span>
            <select
              className="select mono"
              aria-label="Длительность доли"
              disabled={!ts}
              value={ts ? String(ts[1]) : '4'}
              onChange={(e) => ts && patchSong({ time_sig: [ts[0], Number(e.target.value)] })}
            >
              {TIME_SIG_DENOMINATORS.map((d) => (
                <option key={d} value={d}>
                  {d}
                </option>
              ))}
            </select>
          </span>
        </label>
        <label className="field">
          <span>Тональность</span>
          <input className="input" value={song.key ?? ''} placeholder="E minor" onChange={(e) => patchSong({ key: e.target.value })} />
        </label>
      </div>
      <label className="field">
        <span>Заметки</span>
        <textarea
          className="input song-ed-notes"
          rows={2}
          maxLength={SONG_LIMITS.notes}
          value={song.notes ?? ''}
          onChange={(e) => patchSong({ notes: e.target.value })}
        />
      </label>

      <div className="song-ed-keys">
        <label className="row gap">
          <input
            type="checkbox"
            checked={!!ck}
            onChange={(e) => patchSong({ control_keys: e.target.checked ? { ...DEFAULT_CONTROL_KEYS, enabled: true } : undefined })}
          />
          <span>Свои клавиши переключения</span>
        </label>
        {ck && (
          <span className="row gap">
            <label className="row gap">
              <input type="checkbox" checked={ck.enabled} onChange={(e) => patchSong({ control_keys: { ...ck, enabled: e.target.checked } })} />
              <span>вкл.</span>
            </label>
            <span className="muted">◀</span>
            <NoteSelect label="Клавиша «назад»" value={ck.prev} onChange={(prev) => patchSong({ control_keys: { ...ck, prev } })} />
            <span className="muted">▶</span>
            <NoteSelect label="Клавиша «вперёд»" value={ck.next} onChange={(next) => patchSong({ control_keys: { ...ck, next } })} />
          </span>
        )}
      </div>

      <div className="song-ed-sections-head">
        <span className="panel-label">Секции</span>
        <span className="muted mono">
          {song.sections.length}/{SONG_LIMITS.sections}
        </span>
      </div>
      <ol className="song-ed-sections">
        {song.sections.map((s, i) => (
          <SectionRow
            key={i}
            section={s}
            index={i}
            count={song.sections.length}
            presets={presets}
            patterns={patterns}
            onChange={(p) => setSong((x) => updateSection(x, i, p))}
            onMove={(to) => setSong((x) => moveSection(x, i, to))}
            onRemove={() => setSong((x) => removeSection(x, i))}
          />
        ))}
      </ol>
      <button
        type="button"
        className="btn small"
        disabled={song.sections.length >= SONG_LIMITS.sections}
        onClick={() => setSong((x) => addSection(x))}
      >
        + Секция
      </button>

      {tried && issues.length > 0 && (
        <ul className="song-ed-issues" role="alert">
          {issues.map((x) => (
            <li key={x.field}>{x.message}</li>
          ))}
        </ul>
      )}
      {error && (
        <p className="song-ed-error" role="alert">
          {error}
        </p>
      )}
      {confirm ? (
        <div className="song-ed-actions">
          <span>Песня с таким именем уже есть. Заменить?</span>
          <button
            type="button"
            className="btn small primary"
            disabled={busy}
            onClick={() => {
              setConfirm(false);
              void save(true);
            }}
          >
            Заменить
          </button>
          <button type="button" className="btn small" onClick={() => setConfirm(false)}>
            Отмена
          </button>
        </div>
      ) : (
        <div className="song-ed-actions">
          <button type="button" className="btn primary" disabled={busy} onClick={() => void save(false)}>
            {origin === 'factory' ? 'Сохранить копию' : 'Сохранить'}
          </button>
          <button type="button" className="btn" onClick={onCancel}>
            Отмена
          </button>
        </div>
      )}
    </div>
  );
}
