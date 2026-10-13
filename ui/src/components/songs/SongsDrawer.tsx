// Songs drawer (setlists, ARCHITECTURE §18): active song with its sections, the song list (user / factory), the
// editor and the global control keys.

import { useEffect, useRef, useState } from 'react';
import { songActions, useSongs } from '../../store';
import type { Song, SongInfo } from '../../protocol/types';
import { currentSection } from '../../store/songStore';
import { newSong, type SongOrigin } from '../../lib/songEdit';
import { noteName } from '../../lib/notes';
import { SongEditor } from './SongEditor';
import { SongSections } from './SongSections';
import { NoteSelect } from './NoteSelect';

function ActiveSong() {
  const state = useSongs((s) => s.state);
  const song = state?.song;
  if (!state || !song) return null;
  const sec = currentSection(state);
  const ck = state.controlKeys;
  return (
    <section className="song-card song-active" aria-label="Текущая песня">
      <div className="song-card-head">
        <span className="panel-label">Играет</span>
        <span className="song-title">{song.title}</span>
        {song.artist && <span className="muted">{song.artist}</span>}
        <span className="spacer" />
        <button type="button" className="btn small" onClick={() => void songActions().close()}>
          Закрыть
        </button>
      </div>
      <SongSections
        sections={song.sections}
        current={state.section}
        onSection={(i) => void songActions().section(i)}
        onPrev={() => void songActions().prev()}
        onNext={() => void songActions().next()}
      />
      {sec?.notes && <p className="song-notes">{sec.notes}</p>}
      {ck.enabled && (
        <p className="hint">
          Клавиши: ◀ {ck.prev === null ? '—' : noteName(ck.prev)} · ▶ {ck.next === null ? '—' : noteName(ck.next)}
          {ck.source === 'song' ? ' (песня)' : ''}
        </p>
      )}
    </section>
  );
}

function SongRow({ info, active, onEdit }: { info: SongInfo; active: boolean; onEdit(): void }) {
  const [confirm, setConfirm] = useState(false);
  return (
    <li className={`song-row${active ? ' current' : ''}`}>
      <button type="button" className="song-row-main" onClick={() => void songActions().select(info.path)} title="Играть">
        <span className="song-row-title">{info.title}</span>
        <span className="song-row-meta muted">
          {info.artist ? `${info.artist} · ` : ''}
          {info.sections} секц.
        </span>
      </button>
      {confirm ? (
        <>
          <button
            type="button"
            className="btn small danger"
            onClick={() => {
              setConfirm(false);
              void songActions().remove(info.path);
            }}
          >
            Удалить
          </button>
          <button type="button" className="btn small" onClick={() => setConfirm(false)}>
            Нет
          </button>
        </>
      ) : (
        <>
          <button type="button" className="btn small" onClick={onEdit}>
            {info.factory ? 'Копия' : 'Изменить'}
          </button>
          {!info.factory && (
            <button type="button" className="icon-btn" aria-label="Удалить песню" title="Удалить" onClick={() => setConfirm(true)}>
              ×
            </button>
          )}
        </>
      )}
    </li>
  );
}

function ControlKeysCard() {
  const g = useSongs((s) => s.state?.globalControlKeys);
  if (!g) return null;
  return (
    <section className="song-card" aria-label="Клавиши переключения">
      <div className="song-card-head">
        <span className="panel-label">Клавиши переключения</span>
      </div>
      <div className="row gap song-keys">
        <label className="row gap">
          <input type="checkbox" checked={g.enabled} onChange={(e) => void songActions().setControlKeys({ enabled: e.target.checked })} />
          <span>вкл.</span>
        </label>
        <span className="muted">◀</span>
        <NoteSelect label="Клавиша «назад»" value={g.prev} onChange={(prev) => void songActions().setControlKeys({ prev })} />
        <span className="muted">▶</span>
        <NoteSelect label="Клавиша «вперёд»" value={g.next} onChange={(next) => void songActions().setControlKeys({ next })} />
      </div>
      <p className="hint">Пока выбрана песня, эти клавиши не звучат и переключают секции.</p>
    </section>
  );
}

export function SongsDrawer({ open, onClose }: { open: boolean; onClose: () => void }) {
  const ref = useRef<HTMLDialogElement>(null);
  const songs = useSongs((s) => s.songs);
  const loaded = useSongs((s) => s.loaded);
  const activePath = useSongs((s) => s.state?.path ?? null);
  const error = useSongs((s) => s.error);
  const [editing, setEditing] = useState<{ song: Song; origin: SongOrigin } | null>(null);

  useEffect(() => {
    const d = ref.current;
    if (!d) return;
    if (open && !d.open) {
      d.showModal();
      void songActions().refresh();
    } else if (!open && d.open) d.close();
  }, [open]);

  const edit = async (info: SongInfo) => {
    const song = await songActions().get(info.path);
    if (song) setEditing({ song, origin: info.factory ? 'factory' : 'user' });
  };

  const user = songs.filter((s) => !s.factory);
  const factory = songs.filter((s) => s.factory);
  const list = (items: SongInfo[]) => (
    <ul className="song-list">
      {items.map((s) => (
        <SongRow key={s.path} info={s} active={s.path === activePath} onEdit={() => void edit(s)} />
      ))}
    </ul>
  );

  return (
    <dialog
      ref={ref}
      className="drawer songs-drawer"
      onClose={onClose}
      onCancel={onClose}
      onClick={(e) => {
        if (e.target === ref.current) onClose();
      }}
      aria-label="Песни"
    >
      <header className="dialog-head">
        <h2>Песни</h2>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Закрыть">
          ×
        </button>
      </header>
      <div className="drawer-body">
        {open &&
          (editing ? (
            <SongEditor
              initial={editing.song}
              origin={editing.origin}
              onCancel={() => setEditing(null)}
              onDone={() => setEditing(null)}
            />
          ) : (
            <>
              <ActiveSong />
              {error && (
                <p className="song-ed-error" role="alert" onClick={() => songActions().clearError()}>
                  {error}
                </p>
              )}
              <section className="song-card">
                <div className="song-card-head">
                  <span className="panel-label">Мои</span>
                  <span className="spacer" />
                  <button type="button" className="btn small" onClick={() => setEditing({ song: newSong(), origin: 'new' })}>
                    + Песня
                  </button>
                </div>
                {user.length ? list(user) : <p className="hint">{loaded ? 'Нет своих песен.' : 'Загрузка…'}</p>}
              </section>
              {factory.length > 0 && (
                <section className="song-card">
                  <div className="song-card-head">
                    <span className="panel-label">Заводские</span>
                  </div>
                  {list(factory)}
                </section>
              )}
              <ControlKeysCard />
            </>
          ))}
      </div>
    </dialog>
  );
}
