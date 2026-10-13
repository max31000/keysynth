// Song editor helpers (lib/songEdit.ts): sections, validation, the song_save payload (PRESETS.md "Songs").
import { describe, expect, it } from 'vitest';
import {
  addSection,
  drumsMode,
  drumsValue,
  isSlug,
  moveSection,
  newSong,
  removeSection,
  savePayload,
  slugify,
  updateSection,
  updateSong,
  validateSong,
} from '../src/lib/songEdit';
import type { Song } from '../src/protocol/types';

const song = (o: Partial<Song> = {}): Song => ({
  format: 1,
  id: 'riders-on-the-storm',
  title: 'Riders on the Storm',
  sections: [{ name: 'Intro' }, { name: 'Verse' }, { name: 'Solo' }],
  ...o,
});
const names = (s: Song) => s.sections.map((x) => x.name);

describe('sections', () => {
  it('add at the end or at an index, ≤ 64', () => {
    const s = song();
    expect(names(addSection(s))).toHaveLength(4);
    expect(names(addSection(s, 1, { name: 'Pre' }))).toEqual(['Intro', 'Pre', 'Verse', 'Solo']);
    expect(s.sections).toHaveLength(3); // immutable
    const full = song({ sections: Array.from({ length: 64 }, (_, i) => ({ name: `S${i}` })) });
    expect(addSection(full)).toBe(full);
  });

  it('remove and reorder', () => {
    const s = song();
    expect(names(removeSection(s, 1))).toEqual(['Intro', 'Solo']);
    expect(removeSection(s, 5)).toBe(s);
    expect(names(moveSection(s, 0, 2))).toEqual(['Verse', 'Solo', 'Intro']);
    expect(names(moveSection(s, 2, 1))).toEqual(['Intro', 'Solo', 'Verse']);
    expect(moveSection(s, 0, 3)).toBe(s);
    expect(moveSection(s, 1, 1)).toBe(s);
  });

  it('update: undefined removes a field (absent = keep), others set it; unknown keys survive', () => {
    const s = song({ sections: [{ name: 'A', preset: 'x', drums: true, bars: 8, future: { a: 1 } }] });
    const u = updateSection(s, 0, { preset: undefined, drums: drumsValue('keep'), tempo: 90 });
    expect(u.sections[0]).toEqual({ name: 'A', tempo: 90, bars: 8, future: { a: 1 } });
    expect(updateSong(song({ tempo: 100, notation: { musicxml: 'p' } }), { tempo: undefined })).toEqual(
      song({ notation: { musicxml: 'p' } }),
    );
  });

  it('drums tri-state', () => {
    expect(drumsMode({ name: 'a' })).toBe('keep');
    expect(drumsMode({ name: 'a', drums: true })).toBe('start');
    expect(drumsMode({ name: 'a', drums: false })).toBe('stop');
    expect(drumsValue('start')).toBe(true);
    expect(drumsValue('stop')).toBe(false);
    expect(drumsValue('keep')).toBeUndefined();
  });
});

describe('validateSong', () => {
  const fields = (s: Song) => validateSong(s).map((i) => i.field);
  it('a good song has no issues', () => {
    expect(validateSong(song({ tempo: 104, time_sig: [7, 8], control_keys: { enabled: true, prev: 21, next: null } }))).toEqual([]);
  });
  it('flags title, tempo, meter, sections, section fields, control keys', () => {
    expect(fields(song({ title: '  ' }))).toEqual(['title']);
    expect(fields(song({ title: 'x'.repeat(201) }))).toEqual(['title']);
    expect(fields(song({ tempo: 19 }))).toEqual(['tempo']);
    expect(fields(song({ tempo: 401 }))).toEqual(['tempo']);
    expect(fields(song({ time_sig: [4, 3] }))).toEqual(['time_sig']);
    expect(fields(song({ time_sig: [17, 4] }))).toEqual(['time_sig']);
    expect(fields(song({ sections: [] }))).toEqual(['sections']);
    expect(fields(song({ sections: [{ name: '' }, { name: 'ok', tempo: 500 }, { name: 'x'.repeat(61) }] }))).toEqual([
      'sections.0.name',
      'sections.1.tempo',
      'sections.2.name',
    ]);
    expect(fields(song({ sections: [{ name: 'a', notes: 'x'.repeat(2001) }] }))).toEqual(['sections.0.notes']);
    expect(fields(song({ control_keys: { enabled: true, prev: 128, next: 22 } }))).toEqual(['control_keys']);
  });
  it('a new song needs a title', () => {
    expect(fields(newSong())).toEqual(['title']);
  });
});

describe('savePayload', () => {
  it('trims, drops empty optional texts and empty preset, keeps reserved / unknown keys', () => {
    const s = song({
      title: '  Riders on the Storm ',
      artist: ' ',
      key: 'E minor',
      notes: '',
      notation: { musicxml: 'presets/songs/x.musicxml' },
      sections: [
        { name: ' Intro ', preset: '', notes: '  ', start_bar: 1 },
        { name: 'Verse', preset: 'warm-rhodes', pattern: '', drums: true, notes: 'Em7 - A' },
      ],
    });
    const { song: out, overwrite } = savePayload(s, 'user');
    expect(overwrite).toBe(true);
    expect(out).toEqual({
      format: 1,
      id: 'riders-on-the-storm',
      title: 'Riders on the Storm',
      key: 'E minor',
      notation: { musicxml: 'presets/songs/x.musicxml' },
      sections: [
        { name: 'Intro', start_bar: 1 },
        { name: 'Verse', preset: 'warm-rhodes', pattern: '', drums: true, notes: 'Em7 - A' },
      ],
    });
  });

  it('new song: id from the title, no overwrite; factory copy keeps its id', () => {
    const fresh = { ...newSong(), title: 'Light My Fire' };
    expect(savePayload(fresh, 'new')).toEqual({ song: { ...fresh, id: 'light-my-fire' } });
    const f = savePayload(song(), 'factory');
    expect(f.song.id).toBe('riders-on-the-storm');
    expect(f).not.toHaveProperty('overwrite');
  });

  it('a title without a slug gets a unique id', () => {
    const r = savePayload({ ...newSong(), title: 'Песня' }, 'new', 1_700_000_000_000);
    expect(r.song.id).toBe(`song-${(1_700_000_000_000).toString(36)}`);
    expect(isSlug(r.song.id)).toBe(true);
  });

  it('slugify / isSlug', () => {
    expect(slugify('Riders on the Storm!')).toBe('riders-on-the-storm');
    expect(slugify('—')).toBe('');
    expect(slugify('a'.repeat(70)).length).toBe(64);
    expect(isSlug('a-b-1')).toBe(true);
    expect(isSlug('A-b')).toBe(false);
    expect(isSlug('a--b')).toBe(false);
  });
});
