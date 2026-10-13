// @vitest-environment jsdom
// Пульт song mode: the section buttons (components/songs/SongSections.tsx) wired to the song store as RemoteView
// does → song_section / song_prev / song_next on the wire; the current section is highlighted.
import { afterEach, describe, expect, it } from 'vitest';
import { act, createElement } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { EngineClient } from '../src/protocol/client';
import { createSongStore } from '../src/store/songStore';
import { SongSections } from '../src/components/songs/SongSections';
import type { SongSection } from '../src/protocol/types';
import { FakeSocket, latestSocket } from './fakeSocket';

(globalThis as { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true;

const SECTIONS: SongSection[] = [{ name: 'Intro' }, { name: 'Verse' }, { name: 'Solo' }];
let root: Root | null = null;
let host: HTMLDivElement | null = null;

afterEach(() => {
  act(() => root?.unmount());
  host?.remove();
  root = null;
});

function mount(current: number) {
  if (root) {
    act(() => root!.unmount());
    host?.remove();
  }
  FakeSocket.all = [];
  const client = new EngineClient({ url: 'ws://t', createSocket: (u) => new FakeSocket(u), jitter: 0 });
  const store = createSongStore({ client });
  client.connect();
  latestSocket().open();
  host = document.createElement('div');
  document.body.appendChild(host);
  root = createRoot(host);
  const s = store.getState();
  act(() =>
    root!.render(
      createElement(SongSections, {
        big: true,
        sections: SECTIONS,
        current,
        onSection: (i: number) => void s.section(i),
        onPrev: () => void s.prev(),
        onNext: () => void s.next(),
      }),
    ),
  );
  const buttons = [...host.querySelectorAll('button')];
  return { sock: latestSocket(), buttons, sections: buttons.filter((b) => b.classList.contains('song-section')) };
}

describe('Пульт song mode', () => {
  it('one big button per section, the current one highlighted', () => {
    const { sections } = mount(1);
    expect(sections.map((b) => b.textContent)).toEqual(['1Intro', '2Verse', '3Solo']);
    expect(sections.map((b) => b.getAttribute('aria-pressed'))).toEqual(['false', 'true', 'false']);
    expect(sections[1]!.classList.contains('current')).toBe(true);
  });

  it('a section button sends song_section with its index', () => {
    const { sock, sections } = mount(0);
    act(() => sections[2]!.click());
    expect(sock.last()).toMatchObject({ type: 'song_section', index: 2 });
  });

  it('prev / next send song_prev / song_next; disabled at the ends', () => {
    const { sock, buttons } = mount(1);
    const prev = buttons.find((b) => b.getAttribute('aria-label') === 'Предыдущая секция')!;
    const next = buttons.find((b) => b.getAttribute('aria-label') === 'Следующая секция')!;
    act(() => next.click());
    expect(sock.last().type).toBe('song_next');
    act(() => prev.click());
    expect(sock.last().type).toBe('song_prev');
    const first = mount(0);
    expect(first.buttons.find((b) => b.getAttribute('aria-label') === 'Предыдущая секция')!.disabled).toBe(true);
    const last = mount(2);
    expect(last.buttons.find((b) => b.getAttribute('aria-label') === 'Следующая секция')!.disabled).toBe(true);
  });
});
