// Section buttons of the active song + prev / next. Presentational (no store access): the song bar, the songs
// drawer and the Пульт song mode pass the song_* actions in.

import type { SongSection } from '../../protocol/types';

export interface SongSectionsProps {
  sections: SongSection[];
  /** current section index (-1 = none) */
  current: number;
  onSection(index: number): void;
  onPrev(): void;
  onNext(): void;
  /** big touch buttons (Пульт) */
  big?: boolean;
  disabled?: boolean;
}

export function SongSections({ sections, current, onSection, onPrev, onNext, big, disabled }: SongSectionsProps) {
  return (
    <div className={`song-sections${big ? ' big' : ''}`}>
      <button
        type="button"
        className="song-step"
        aria-label="Предыдущая секция"
        disabled={disabled || current <= 0}
        onClick={onPrev}
      >
        ◀
      </button>
      <div className="song-section-list" role="group" aria-label="Секции">
        {sections.map((s, i) => (
          <button
            key={i}
            type="button"
            className={`song-section${i === current ? ' current' : ''}`}
            aria-pressed={i === current}
            disabled={disabled}
            onClick={() => onSection(i)}
          >
            <span className="song-section-num mono">{i + 1}</span>
            <span className="song-section-name">{s.name}</span>
          </button>
        ))}
      </div>
      <button
        type="button"
        className="song-step"
        aria-label="Следующая секция"
        disabled={disabled || current >= sections.length - 1}
        onClick={onNext}
      >
        ▶
      </button>
    </div>
  );
}
