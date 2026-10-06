import { useMemo, useState } from 'react';
import { actions, useEngine } from '../store';
import type { PresetEntry } from '../protocol/types';
import { useFavourites } from '../lib/favourites';

/** ARCHITECTURE §9 category list (order), extended by whatever the engine reports. */
const CATEGORY_ORDER = [
  'Piano',
  'E.Piano',
  'Organ',
  'Synth Lead',
  'Synth Pad',
  'Synth Bass',
  'Brass',
  'Strings',
  'Bells & Keys',
  'Choir & Vox',
  'Drums',
  'FX',
  'Splits & Layers',
];

const ALL = '__all';
const FAVS = '__favs';
const USER = '__user';

function matches(p: PresetEntry, q: string) {
  if (!q) return true;
  const hay = `${p.name} ${p.category} ${p.tags.join(' ')}`.toLowerCase();
  return q
    .toLowerCase()
    .split(/\s+/)
    .filter(Boolean)
    .every((w) => hay.includes(w));
}

function SaveAs({ onDone }: { onDone: () => void }) {
  const meta = useEngine((s) => s.patch?.meta);
  const [name, setName] = useState(meta?.name ?? '');
  const [category, setCategory] = useState(meta?.category ?? 'Synth Lead');
  const [overwrite, setOverwrite] = useState(false);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    if (!name.trim()) return;
    setBusy(true);
    const path = await actions().savePreset(name.trim(), category, overwrite);
    setBusy(false);
    if (path) onDone();
  };
  return (
    <form
      className="save-as"
      onSubmit={(e) => {
        e.preventDefault();
        void submit();
      }}
    >
      <div className="panel-label">Save to userdata</div>
      <input className="input" placeholder="Preset name" value={name} autoFocus onChange={(e) => setName(e.target.value)} />
      <select className="select" value={category} onChange={(e) => setCategory(e.target.value)}>
        {CATEGORY_ORDER.map((c) => (
          <option key={c}>{c}</option>
        ))}
      </select>
      <label className="check">
        <input type="checkbox" checked={overwrite} onChange={(e) => setOverwrite(e.target.checked)} /> Overwrite existing
      </label>
      <div className="row gap">
        <button type="submit" className="btn primary" disabled={busy || !name.trim()}>
          {busy ? 'Saving…' : 'Save'}
        </button>
        <button type="button" className="btn" onClick={onDone}>
          Cancel
        </button>
      </div>
    </form>
  );
}

export function PresetBrowser() {
  const presets = useEngine((s) => s.presets);
  const presetPath = useEngine((s) => s.presetPath);
  const dirty = useEngine((s) => s.dirty);
  const patchName = useEngine((s) => s.patch?.meta.name ?? '—');
  const [favs, toggleFav] = useFavourites();
  const [cat, setCat] = useState<string>(ALL);
  const [query, setQuery] = useState('');
  const [tag, setTag] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const categories = useMemo(() => {
    const present = new Set(presets.map((p) => p.category));
    const ordered = CATEGORY_ORDER.filter((c) => present.has(c));
    const extra = [...present].filter((c) => !CATEGORY_ORDER.includes(c)).sort();
    return [...ordered, ...extra];
  }, [presets]);

  const counts = useMemo(() => {
    const m = new Map<string, number>();
    for (const p of presets) m.set(p.category, (m.get(p.category) ?? 0) + 1);
    return m;
  }, [presets]);

  const scoped = useMemo(
    () =>
      presets.filter((p) =>
        cat === ALL ? true : cat === FAVS ? favs.has(p.path) : cat === USER ? !p.factory : p.category === cat,
      ),
    [presets, cat, favs],
  );

  const topTags = useMemo(() => {
    const m = new Map<string, number>();
    for (const p of scoped) for (const t of p.tags) m.set(t, (m.get(t) ?? 0) + 1);
    return [...m.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0])).slice(0, 14).map(([t]) => t);
  }, [scoped]);

  const list = useMemo(
    () =>
      scoped
        .filter((p) => matches(p, query) && (!tag || p.tags.includes(tag)))
        .sort((a, b) => a.name.localeCompare(b.name)),
    [scoped, query, tag],
  );

  const catBtn = (id: string, label: string, n?: number) => (
    <button
      type="button"
      key={id}
      className={`cat ${cat === id ? 'on' : ''}`}
      onClick={() => {
        setCat(id);
        setTag(null);
      }}
    >
      <span>{label}</span>
      {n !== undefined && <span className="cat-n mono">{n}</span>}
    </button>
  );

  return (
    <aside className="browser panel">
      <div className="current-preset">
        <div className="panel-label">Patch</div>
        <div className="current-name" title={presetPath ?? 'unsaved'}>
          {patchName}
          {dirty && <span className="dirty" title="Modified">●</span>}
        </div>
        <div className="row gap">
          <button type="button" className="btn small" onClick={() => setSaving((v) => !v)}>
            Save as…
          </button>
          <button type="button" className="btn small ghost" onClick={() => void actions().refreshPresets()} title="Reload preset list">
            ↻
          </button>
        </div>
        {saving && <SaveAs onDone={() => setSaving(false)} />}
      </div>

      <div className="search">
        <svg viewBox="0 0 16 16" width="13" height="13" aria-hidden>
          <circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" strokeWidth="1.5" />
          <path d="M10.5 10.5 L14 14" stroke="currentColor" strokeWidth="1.5" />
        </svg>
        <input
          className="input"
          placeholder="Search presets, tags…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          onKeyDown={(e) => e.key === 'Escape' && setQuery('')}
        />
      </div>

      <nav className="cats" aria-label="Categories">
        {catBtn(ALL, 'All', presets.length)}
        {catBtn(FAVS, '★ Favourites', presets.filter((p) => favs.has(p.path)).length)}
        {catBtn(USER, 'User', presets.filter((p) => !p.factory).length)}
        <div className="cats-sep" />
        {categories.map((c) => catBtn(c, c, counts.get(c)))}
      </nav>

      {topTags.length > 0 && (
        <div className="tags">
          {topTags.map((t) => (
            <button type="button" key={t} className={`tag ${tag === t ? 'on' : ''}`} onClick={() => setTag(tag === t ? null : t)}>
              {t}
            </button>
          ))}
        </div>
      )}

      <ul className="preset-list" role="listbox" aria-label="Presets">
        {list.map((p) => (
          <li
            key={p.path}
            role="option"
            aria-selected={p.path === presetPath}
            className={`preset ${p.path === presetPath ? 'on' : ''}`}
            onClick={() => void actions().loadPreset(p.path)}
            onKeyDown={(e) => e.key === 'Enter' && void actions().loadPreset(p.path)}
            tabIndex={0}
            title={p.path}
          >
            <button
              type="button"
              className={`fav ${favs.has(p.path) ? 'on' : ''}`}
              aria-label={favs.has(p.path) ? 'Remove favourite' : 'Add favourite'}
              onClick={(e) => {
                e.stopPropagation();
                toggleFav(p.path);
              }}
            >
              {favs.has(p.path) ? '★' : '☆'}
            </button>
            <div className="preset-main">
              <div className="preset-name">{p.name}</div>
              <div className="preset-sub">
                {cat === ALL || cat === FAVS || cat === USER ? <span>{p.category}</span> : null}
                {p.tags.slice(0, 3).map((t) => (
                  <span key={t} className="preset-tag">
                    {t}
                  </span>
                ))}
              </div>
            </div>
            {!p.factory && <span className="badge">USER</span>}
          </li>
        ))}
        {list.length === 0 && <li className="empty">No presets match.</li>}
      </ul>
    </aside>
  );
}
