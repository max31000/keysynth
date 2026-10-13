// Song editor helpers (schema and limits: PRESETS.md "Songs"). Pure and immutable; unknown / reserved keys of the
// song and its sections are carried along untouched (the engine keeps them verbatim).

import type { ControlKeys, Song, SongSection } from '../protocol/types';

export const SONG_LIMITS = {
  title: 200,
  sectionName: 60,
  notes: 2000,
  sections: 64,
  tempoMin: 20,
  tempoMax: 400,
  idMax: 64,
} as const;
export const TIME_SIG_DENOMINATORS = [2, 4, 8, 16] as const;
export const DEFAULT_CONTROL_KEYS: ControlKeys = { enabled: false, prev: 21, next: 22 };

const SLUG_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;
export const isSlug = (s: unknown): s is string => typeof s === 'string' && s.length <= SONG_LIMITS.idMax && SLUG_RE.test(s);

/** File-name slug of a title ("Riders on the Storm" → "riders-on-the-storm"); "" when nothing is left. */
export function slugify(s: string): string {
  return s
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, SONG_LIMITS.idMax)
    .replace(/-+$/g, '');
}

export function newSection(name = 'Секция'): SongSection {
  return { name };
}

export function newSong(): Song {
  return { format: 1, id: '', title: '', sections: [newSection('Интро')] };
}

/** Drop keys whose patch value is `undefined` (= field absent), set the others. */
function patchObject<T extends object>(o: T, patch: Partial<T>): T {
  const next = { ...o } as Record<string, unknown>;
  for (const [k, v] of Object.entries(patch)) {
    if (v === undefined) delete next[k];
    else next[k] = v;
  }
  return next as T;
}

export const updateSong = (song: Song, patch: Partial<Song>): Song => patchObject(song, patch);

export function updateSection(song: Song, index: number, patch: Partial<SongSection>): Song {
  if (index < 0 || index >= song.sections.length) return song;
  return { ...song, sections: song.sections.map((s, i) => (i === index ? patchObject(s, patch) : s)) };
}

/** Insert a section (default: a new one with only a name) at `at` (default: the end); ≤ 64 sections. */
export function addSection(song: Song, at = song.sections.length, section?: SongSection): Song {
  if (song.sections.length >= SONG_LIMITS.sections) return song;
  const i = Math.max(0, Math.min(at, song.sections.length));
  const sections = song.sections.slice();
  sections.splice(i, 0, section ?? newSection(`Секция ${song.sections.length + 1}`));
  return { ...song, sections };
}

export function removeSection(song: Song, index: number): Song {
  if (index < 0 || index >= song.sections.length) return song;
  return { ...song, sections: song.sections.filter((_, i) => i !== index) };
}

export function moveSection(song: Song, from: number, to: number): Song {
  const n = song.sections.length;
  if (from < 0 || from >= n || to < 0 || to >= n || from === to) return song;
  const sections = song.sections.slice();
  const [s] = sections.splice(from, 1);
  sections.splice(to, 0, s!);
  return { ...song, sections };
}

/** `drums` tri-state for the editor. */
export type DrumsMode = 'keep' | 'start' | 'stop';
export const drumsMode = (s: SongSection): DrumsMode => (s.drums === true ? 'start' : s.drums === false ? 'stop' : 'keep');
export const drumsValue = (m: DrumsMode): boolean | undefined => (m === 'start' ? true : m === 'stop' ? false : undefined);

export interface SongIssue {
  /** `title`, `tempo`, `sections`, `sections.2.name`, … */
  field: string;
  message: string;
}

const tempoOk = (t: unknown) =>
  typeof t === 'number' && Number.isFinite(t) && t >= SONG_LIMITS.tempoMin && t <= SONG_LIMITS.tempoMax;
const noteOk = (n: unknown) => n === null || (typeof n === 'number' && Number.isInteger(n) && n >= 0 && n <= 127);

/** Problems the engine would reject (`bad_request`); empty = savable. Messages are short Russian UI texts. */
export function validateSong(song: Song): SongIssue[] {
  const out: SongIssue[] = [];
  const title = song.title.trim();
  if (!title) out.push({ field: 'title', message: 'Нужно название' });
  else if (title.length > SONG_LIMITS.title) out.push({ field: 'title', message: `Название длиннее ${SONG_LIMITS.title} символов` });
  if (song.tempo !== undefined && !tempoOk(song.tempo))
    out.push({ field: 'tempo', message: `Темп ${SONG_LIMITS.tempoMin}–${SONG_LIMITS.tempoMax}` });
  if (song.time_sig !== undefined) {
    const [num, den] = song.time_sig;
    if (!(Number.isInteger(num) && num >= 1 && num <= 16) || !(TIME_SIG_DENOMINATORS as readonly number[]).includes(den))
      out.push({ field: 'time_sig', message: 'Размер: 1–16 / 2, 4, 8, 16' });
  }
  if ((song.notes ?? '').length > SONG_LIMITS.notes) out.push({ field: 'notes', message: `Заметки длиннее ${SONG_LIMITS.notes} символов` });
  const ck = song.control_keys;
  if (ck && (!noteOk(ck.prev) || !noteOk(ck.next))) out.push({ field: 'control_keys', message: 'Клавиши: ноты 0–127' });
  if (song.sections.length === 0) out.push({ field: 'sections', message: 'Нужна хотя бы одна секция' });
  if (song.sections.length > SONG_LIMITS.sections) out.push({ field: 'sections', message: `Не больше ${SONG_LIMITS.sections} секций` });
  song.sections.forEach((s, i) => {
    const name = s.name.trim();
    if (!name) out.push({ field: `sections.${i}.name`, message: `Секция ${i + 1}: нужно название` });
    else if (name.length > SONG_LIMITS.sectionName)
      out.push({ field: `sections.${i}.name`, message: `Секция ${i + 1}: название длиннее ${SONG_LIMITS.sectionName} символов` });
    if (s.tempo !== undefined && !tempoOk(s.tempo))
      out.push({ field: `sections.${i}.tempo`, message: `Секция ${i + 1}: темп ${SONG_LIMITS.tempoMin}–${SONG_LIMITS.tempoMax}` });
    if ((s.notes ?? '').length > SONG_LIMITS.notes)
      out.push({ field: `sections.${i}.notes`, message: `Секция ${i + 1}: заметки длиннее ${SONG_LIMITS.notes} символов` });
  });
  return out;
}

/** Trimmed optional text: absent when empty. */
const optText = (v: unknown): string | undefined => {
  if (typeof v !== 'string') return undefined;
  const t = v.trim();
  return t ? t : undefined;
};

/** Where the edited song came from: a new song, a factory song (saved as a user copy) or a user song. */
export type SongOrigin = 'new' | 'factory' | 'user';

/**
 * `song_save` payload. Empty optional texts are dropped, `id` = the song's id when it is a valid slug (a factory song
 * keeps its id: the copy goes to `userdata/songs/<id>.json`), else the slugified title. A user song overwrites its
 * own file; a new song / factory copy does not (`exists` → the UI asks, then resends with `overwrite`).
 */
export function savePayload(song: Song, origin: SongOrigin, now = Date.now()): { song: Song; overwrite?: boolean } {
  const s: Record<string, unknown> = { ...song, format: song.format || 1, title: song.title.trim() };
  for (const k of ['artist', 'key', 'notes'] as const) {
    const t = optText(song[k]);
    if (t === undefined) delete s[k];
    else s[k] = t;
  }
  // a title without Latin letters or digits (Cyrillic) has no slug: a unique one instead
  s.id = isSlug(song.id) ? song.id : slugify(song.title) || `song-${now.toString(36)}`;
  s.sections = song.sections.map((sec) => {
    const o: Record<string, unknown> = { ...sec, name: sec.name.trim() };
    const notes = optText(sec.notes);
    if (notes === undefined) delete o.notes;
    else o.notes = notes;
    if (sec.preset === '') delete o.preset; // "" in the picker = keep the current sound
    return o as SongSection;
  });
  const out: { song: Song; overwrite?: boolean } = { song: s as Song };
  if (origin === 'user') out.overwrite = true;
  return out;
}
