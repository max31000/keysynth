// Fake factory songs for the mock engine (PRESETS.md "Songs"); presets / patterns point at the mock's own lists.

import type { Song } from '../src/protocol/types';

export interface MockSong {
  path: string;
  factory: boolean;
  song: Song;
}

export function factorySongs(): MockSong[] {
  const songs: Song[] = [
    {
      format: 1,
      id: 'riders-on-the-storm',
      title: 'Riders on the Storm',
      artist: 'The Doors',
      tempo: 104,
      time_sig: [4, 4],
      key: 'E minor',
      notes: 'Rhodes intro, drums from bar 5.',
      sections: [
        { name: 'Intro', preset: 'presets/factory/e-piano/riders-rhodes.json', drums: false },
        { name: 'Verse', preset: 'presets/factory/e-piano/riders-rhodes.json', pattern: 'presets/patterns/riders-shuffle.json', drums: true, notes: 'Em7 - A' },
        { name: 'Solo', preset: 'presets/factory/e-piano/riders-rhodes.json', tempo: 106, notes: 'Em pentatonic, 16 bars' },
        { name: 'Outro', drums: false },
      ],
    },
    {
      format: 1,
      id: 'light-my-fire',
      title: 'Light My Fire',
      artist: 'The Doors',
      tempo: 126,
      time_sig: [4, 4],
      key: 'A minor',
      sections: [
        { name: 'Intro', preset: 'presets/factory/organ/light-my-fire.json', pattern: 'presets/patterns/basic-rock.json', drums: true },
        { name: 'Verse', preset: 'presets/factory/organ/light-my-fire.json', notes: 'Am - F#m' },
        { name: 'Chorus', preset: 'presets/factory/organ/light-my-fire.json' },
        { name: 'Solo', preset: 'presets/factory/organ/echoes-b3.json', notes: 'A dorian' },
      ],
      // reserved field (PRESETS.md): kept verbatim
      notation: { musicxml: 'presets/songs/light-my-fire.musicxml' },
    },
  ];
  return songs.map((song) => ({ path: `presets/songs/${song.id}.json`, factory: true, song }));
}
