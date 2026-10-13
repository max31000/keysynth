// Strip under the top bar while a song is selected: song title, section buttons, prev / next, close.

import { songActions, useSongs } from '../../store';
import { SongSections } from './SongSections';

export function SongBar({ onOpen }: { onOpen: () => void }) {
  const state = useSongs((s) => s.state);
  const song = state?.song;
  if (!state || !song) return null;
  return (
    <div className="song-bar" role="region" aria-label="Песня">
      <button type="button" className="song-bar-title" onClick={onOpen} title="Песни">
        <span className="panel-label">Песня</span>
        <span className="song-title">{song.title}</span>
      </button>
      <SongSections
        sections={song.sections}
        current={state.section}
        onSection={(i) => void songActions().section(i)}
        onPrev={() => void songActions().prev()}
        onNext={() => void songActions().next()}
      />
      <button type="button" className="icon-btn" aria-label="Закрыть песню" title="Закрыть песню" onClick={() => void songActions().close()}>
        ×
      </button>
    </div>
  );
}
