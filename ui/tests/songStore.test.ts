// Songs store (store/songStore.ts): song_state events, re-list triggers, song_* requests (PROTOCOL.md "Songs").
import { describe, expect, it } from 'vitest';
import { EngineClient } from '../src/protocol/client';
import { createSongStore, currentSection } from '../src/store/songStore';
import type { Song, SongInfo, SongStateEvent } from '../src/protocol/types';
import { FakeSocket, latestSocket } from './fakeSocket';

const SONG: Song = {
  format: 1,
  id: 'riders',
  title: 'Riders',
  sections: [{ name: 'Intro' }, { name: 'Verse', notes: 'Em7 - A' }, { name: 'Solo' }],
};
const INFO: SongInfo = { path: 'presets/songs/riders.json', id: 'riders', title: 'Riders', artist: '', sections: 3, factory: true };
const KEYS = { enabled: false, prev: 21, next: 22 };
const songState = (o: Partial<SongStateEvent> = {}): SongStateEvent => ({
  type: 'song_state',
  path: INFO.path,
  song: SONG,
  section: 1,
  controlKeys: { ...KEYS, source: 'global' },
  globalControlKeys: KEYS,
  ...o,
});
const HELLO = {
  type: 'state',
  patch: { format: 2, meta: { name: 'x' }, layers: [], master: { volume_db: 0, fx: [] } },
  presetPath: null,
  dirty: false,
  audio: {},
  transport: {},
  version: '0.2.0',
  engine: { product: 'keysynth', kind: 'engine', version: '0.2.0', pid: 1 },
  trust: 'local',
  cloud: { linked: false, accountId: null, accountName: null, deviceId: 'd', expiresAt: null, online: true, error: null },
};

function setup() {
  FakeSocket.all = [];
  const client = new EngineClient({ url: 'ws://t', createSocket: (u) => new FakeSocket(u), jitter: 0 });
  const store = createSongStore({ client });
  client.connect();
  const sock = latestSocket();
  sock.open();
  return { client, store, sock };
}
const tick = () => new Promise((r) => setTimeout(r, 0));
const sentOf = (sock: FakeSocket, type: string) => sock.sent.filter((m) => m.type === type);

describe('song store', () => {
  it('lists songs after the hello reply and keeps song_state', async () => {
    const { store, sock } = setup();
    sock.receive(HELLO);
    expect(sentOf(sock, 'song_list')).toHaveLength(1);
    sock.receive({ type: 'song_list_ok', id: sock.last().id, songs: [INFO] });
    sock.receive(songState());
    await tick();
    const s = store.getState();
    expect(s.loaded).toBe(true);
    expect(s.songs).toEqual([INFO]);
    expect(s.state?.path).toBe(INFO.path);
    expect(s.state?.section).toBe(1);
    expect(currentSection(s.state)?.notes).toBe('Em7 - A');
    expect(s.state).not.toHaveProperty('type');
  });

  it('a closed song: section -1 and no current section', () => {
    const { store, sock } = setup();
    sock.receive(songState({ path: null, song: null, section: -1 }));
    expect(store.getState().state?.song).toBeNull();
    expect(currentSection(store.getState().state)).toBeNull();
  });

  it('does not list from a foreign server', () => {
    const { sock } = setup();
    sock.receive({ ...HELLO, engine: { product: 'other', kind: 'engine', version: '1', pid: 1 } });
    expect(sentOf(sock, 'song_list')).toHaveLength(0);
  });

  it('re-lists on songs_changed', () => {
    const { sock } = setup();
    sock.receive(HELLO);
    sock.receive({ type: 'songs_changed' });
    expect(sentOf(sock, 'song_list')).toHaveLength(2);
  });

  it('re-lists when a sync finished (syncing → idle, new lastSyncAt), not on the first status', () => {
    const { sock } = setup();
    sock.receive(HELLO);
    const sync = (state: string, lastSyncAt: string | null) =>
      sock.receive({ type: 'sync_status', state, lastSyncAt, pending: 0, conflicts: [], error: null });
    sync('idle', '2026-10-13T10:00:00Z'); // first status after hello
    expect(sentOf(sock, 'song_list')).toHaveLength(1);
    sync('syncing', '2026-10-13T10:00:00Z');
    expect(sentOf(sock, 'song_list')).toHaveLength(1);
    sync('idle', '2026-10-13T10:01:00Z');
    expect(sentOf(sock, 'song_list')).toHaveLength(2);
    sync('idle', '2026-10-13T10:01:00Z'); // same status again
    expect(sentOf(sock, 'song_list')).toHaveLength(2);
    sync('idle', '2026-10-13T10:02:00Z'); // a sync we did not see start
    expect(sentOf(sock, 'song_list')).toHaveLength(3);
  });

  it('sends the song_* requests with the documented payloads', async () => {
    const { store, sock } = setup();
    const answer = (type: string) => sock.receive({ type, id: sock.last().id });
    let p: Promise<unknown> = store.getState().select(INFO.path);
    expect(sock.last()).toMatchObject({ type: 'song_select', path: INFO.path });
    expect(sock.last()).not.toHaveProperty('section');
    answer('song_select_ok');
    await p;
    p = store.getState().select(INFO.path, 2);
    expect(sock.last()).toMatchObject({ type: 'song_select', path: INFO.path, section: 2 });
    answer('song_select_ok');
    await p;
    p = store.getState().section(1);
    expect(sock.last()).toMatchObject({ type: 'song_section', index: 1 });
    answer('song_section_ok');
    await p;
    p = store.getState().next();
    expect(sock.last().type).toBe('song_next');
    answer('song_section_ok');
    await p;
    p = store.getState().prev();
    expect(sock.last().type).toBe('song_prev');
    answer('song_section_ok');
    await p;
    p = store.getState().close();
    expect(sock.last().type).toBe('song_close');
    answer('song_close_ok');
    await p;
    p = store.getState().setControlKeys({ enabled: true, next: null });
    expect(sock.last()).toMatchObject({ type: 'song_control_keys', enabled: true, next: null });
    expect(sock.last()).not.toHaveProperty('prev');
    answer('song_control_keys_ok');
    await p;
    expect(store.getState().error).toBeNull();
  });

  it('errors become a panel message (no_song)', async () => {
    const { store, sock } = setup();
    const p = store.getState().next();
    sock.receive({ type: 'error', id: sock.last().id, code: 'no_song', message: 'no song selected' });
    await p;
    expect(store.getState().error).toBe('Песня не выбрана.');
  });

  it('save: overwrite only when asked; `exists` is returned to the caller; a save re-lists', async () => {
    const { store, sock } = setup();
    sock.receive(HELLO);
    sock.receive({ type: 'song_list_ok', id: sock.last().id, songs: [INFO] });
    let p = store.getState().save(SONG);
    expect(sock.last()).toMatchObject({ type: 'song_save', song: SONG });
    expect(sock.last()).not.toHaveProperty('overwrite');
    sock.receive({ type: 'error', id: sock.last().id, code: 'exists', message: 'exists' });
    expect(await p).toEqual({ ok: false, code: 'exists', message: 'exists' });
    p = store.getState().save(SONG, true);
    expect(sock.last()).toMatchObject({ type: 'song_save', overwrite: true });
    sock.receive({ type: 'song_save_ok', id: sock.last().id, path: 'userdata/songs/riders.json', warnings: [] });
    expect(await p).toEqual({ ok: true, path: 'userdata/songs/riders.json' });
    expect(sock.last().type).toBe('song_list');
  });

  it('delete removes the entry and re-lists; get returns the song', async () => {
    const { store, sock } = setup();
    const user: SongInfo = { ...INFO, path: 'userdata/songs/mine.json', factory: false };
    sock.receive(HELLO);
    sock.receive({ type: 'song_list_ok', id: sock.last().id, songs: [INFO, user] });
    const d = store.getState().remove(user.path);
    expect(sock.last()).toMatchObject({ type: 'song_delete', path: user.path });
    sock.receive({ type: 'song_delete_ok', id: sock.last().id, path: user.path });
    expect(await d).toBe(true);
    expect(store.getState().songs.map((s) => s.path)).toEqual([INFO.path]);
    const g = store.getState().get(INFO.path);
    expect(sock.last()).toMatchObject({ type: 'song_get', path: INFO.path });
    sock.receive({ type: 'song_get_ok', id: sock.last().id, path: INFO.path, song: SONG });
    expect(await g).toEqual(SONG);
  });
});
