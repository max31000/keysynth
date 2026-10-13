// Songs (setlists, PROTOCOL.md "Songs"): song list, the engine's `song_state` and the song_* actions.
// Separate from engineStore: it only listens to the same EngineClient. The list is re-requested after the hello
// reply, on `songs_changed`, after a `sync_status` that finished a sync, and on a profile switch (`refresh`).

import { createStore, type StoreApi } from 'zustand/vanilla';
import { EngineError, type EngineClient } from '../protocol/client';
import type { ControlKeys, EngineMsg, Song, SongInfo, SongState } from '../protocol/types';
import { checkIdentity, identityAccepted } from '../platform/identity';

export type SaveResult = { ok: true; path: string } | { ok: false; code: string; message: string };

export interface SongsState {
  songs: SongInfo[];
  /** a `song_list_ok` arrived on this connection */
  loaded: boolean;
  /** last `song_state` (null until the first one) */
  state: SongState | null;
  /** last failed action (shown in the songs panel) */
  error: string | null;

  /** Reducer for one engine message (exposed for tests). */
  handle(msg: EngineMsg): void;
  refresh(): Promise<void>;
  get(path: string): Promise<Song | null>;
  save(song: Song, overwrite?: boolean): Promise<SaveResult>;
  remove(path: string): Promise<boolean>;
  select(path: string, section?: number): Promise<void>;
  section(index: number): Promise<void>;
  next(): Promise<void>;
  prev(): Promise<void>;
  close(): Promise<void>;
  /** global control keys (`settings.json`); only the given fields change */
  setControlKeys(k: Partial<ControlKeys>): Promise<void>;
  clearError(): void;
}

export interface SongDeps {
  client: Pick<EngineClient, 'request' | 'on' | 'onStatus' | 'status'>;
  /** the mock counts as an engine (dev builds, tests); default true */
  acceptMock?: boolean;
}

export type SongStore = StoreApi<SongsState>;

const errText = (e: unknown) => (e instanceof Error ? e.message : String(e));
const codeOf = (e: unknown) => (e instanceof EngineError ? e.code : 'error');

/** The current song's section, or null. */
export const currentSection = (s: SongState | null) =>
  s?.song && s.section >= 0 ? (s.song.sections[s.section] ?? null) : null;

export function songErrorText(code: string, message: string): string {
  switch (code) {
    case 'exists':
      return 'Песня с таким именем уже есть.';
    case 'read_only':
      return 'Заводскую песню нельзя изменить.';
    case 'not_found':
      return 'Песня не найдена.';
    case 'no_song':
      return 'Песня не выбрана.';
    case 'forbidden':
      return 'Недоступно в этом режиме.';
    default:
      return message || code;
  }
}

export function createSongStore(deps: SongDeps): SongStore {
  const { client } = deps;
  /** last sync status seen (re-list when a sync finished) */
  let lastSync: { state: string; lastSyncAt: string | null } | null = null;
  let accepted = false;
  client.onStatus((s) => {
    if (s !== 'open') {
      accepted = false;
      lastSync = null;
    }
  });

  const store = createStore<SongsState>()((set, get) => {
    const run = async (fn: () => Promise<unknown>) => {
      try {
        await fn();
        if (get().error) set({ error: null });
      } catch (e) {
        set({ error: songErrorText(codeOf(e), errText(e)) });
      }
    };

    return {
      songs: [],
      loaded: false,
      state: null,
      error: null,

      handle(msg) {
        switch (msg.type) {
          case 'state':
            // the reply to `hello` (only it carries these): the engine sends `song_state` itself; fetch the list
            if (msg.cloud || msg.version !== undefined || msg.trust) {
              accepted = identityAccepted(checkIdentity(msg), deps.acceptMock ?? true);
              if (accepted) void get().refresh();
            }
            break;
          case 'song_list_ok':
            set({ songs: msg.songs, loaded: true });
            break;
          case 'song_state': {
            const { path, song, section, controlKeys, globalControlKeys } = msg;
            set({ state: { path, song, section, controlKeys, globalControlKeys } });
            break;
          }
          case 'songs_changed':
            void get().refresh();
            break;
          case 'sync_status': {
            const prev = lastSync;
            lastSync = { state: msg.state, lastSyncAt: msg.lastSyncAt };
            const finished =
              !!prev &&
              msg.state !== 'syncing' &&
              (prev.state === 'syncing' || (msg.lastSyncAt !== null && msg.lastSyncAt !== prev.lastSyncAt));
            if (finished) void get().refresh();
            break;
          }
          default:
            break;
        }
      },

      async refresh() {
        if (client.status !== 'open' || !accepted) return;
        await run(() => client.request({ type: 'song_list' }, { expect: 'song_list_ok' }));
      },

      async get(path) {
        try {
          const r = await client.request({ type: 'song_get', path }, { expect: 'song_get_ok' });
          return r.song;
        } catch (e) {
          set({ error: songErrorText(codeOf(e), errText(e)) });
          return null;
        }
      },

      async save(song, overwrite) {
        try {
          const r = await client.request(
            overwrite ? { type: 'song_save', song, overwrite: true } : { type: 'song_save', song },
            { expect: 'song_save_ok' },
          );
          set({ error: null });
          void get().refresh(); // `songs_changed` goes to the others only
          return { ok: true, path: r.path };
        } catch (e) {
          return { ok: false, code: codeOf(e), message: errText(e) };
        }
      },

      async remove(path) {
        try {
          await client.request({ type: 'song_delete', path }, { expect: 'song_delete_ok' });
          set({ error: null, songs: get().songs.filter((s) => s.path !== path) });
          void get().refresh();
          return true;
        } catch (e) {
          set({ error: songErrorText(codeOf(e), errText(e)) });
          return false;
        }
      },

      select: (path, section) =>
        run(() =>
          client.request(
            section !== undefined ? { type: 'song_select', path, section } : { type: 'song_select', path },
            { expect: 'song_select_ok' },
          ),
        ),
      section: (index) => run(() => client.request({ type: 'song_section', index }, { expect: 'song_section_ok' })),
      next: () => run(() => client.request({ type: 'song_next' }, { expect: 'song_section_ok' })),
      prev: () => run(() => client.request({ type: 'song_prev' }, { expect: 'song_section_ok' })),
      close: () => run(() => client.request({ type: 'song_close' }, { expect: 'song_close_ok' })),
      setControlKeys: (k) =>
        run(() => client.request({ type: 'song_control_keys', ...k }, { expect: 'song_control_keys_ok' })),
      clearError: () => set({ error: null }),
    };
  });

  client.on('*', (m) => store.getState().handle(m as EngineMsg));
  return store;
}
