import { describe, expect, it } from 'vitest';
import { EngineClient } from '../src/protocol/client';
import { createEngineStore } from '../src/store/engineStore';
import { applyParam, findModule } from '../src/store/patchOps';
import { emptyPattern, toggleStep } from '../src/lib/stepGrid';
import type { EngineMsg, Patch, StateEvent } from '../src/protocol/types';
import { FakeSocket, latestSocket } from './fakeSocket';

const zone = { key_lo: 0, key_hi: 127, vel_lo: 1, vel_hi: 127, transpose: 0, channel: 0, volume_db: 0, pan: 0, mute: false, solo: false, sustain: true };
const PATCH: Patch = {
  format: 1,
  meta: { name: 'T' },
  layers: [{ node: 1, name: 'A', zone, instrument: { node: 2, type: 'va', params: {} }, fx: [] }],
  master: { volume_db: 0, fx: [] },
  rhythm: { node: 9, kit: { kit: 0, kick_tune: 0 }, pattern: 'presets/patterns/a.json' },
};
const state = (pattern = 'presets/patterns/a.json', time_sig: [number, number] = [4, 4]): StateEvent => ({
  type: 'state',
  patch: PATCH,
  presetPath: null,
  dirty: false,
  audio: { type: 'X', name: 'X', sampleRate: 48000, bufferSize: 64, inputLatencyMs: 1, outputLatencyMs: 1, running: true },
  transport: { playing: false, tempo: 120, metronome: false, metronome_volume: 0.5, pattern, pattern_edited: false, time_sig },
});

function setup() {
  FakeSocket.all = [];
  const client = new EngineClient({ url: 'ws://t', createSocket: (u) => new FakeSocket(u), jitter: 0 });
  client.connect();
  latestSocket().open();
  const queued: (() => void)[] = [];
  const store = createEngineStore({ client, schedule: (fn) => queued.push(fn) });
  client.on('*', (m) => store.getState().handle(m as EngineMsg));
  return { store, sock: latestSocket(), runFrame: () => queued.splice(0).forEach((f) => f()) };
}

describe('rhythm in the store', () => {
  it('rhythm kit is addressable like a module (findModule / applyParam)', () => {
    expect(findModule(PATCH, 9)).toMatchObject({ kind: 'rhythm', slot: { type: 'drums' } });
    const p = applyParam(PATCH, 9, 'kick_tune', 5);
    expect(p.rhythm!.kit!.kick_tune).toBe(5);
    expect(PATCH.rhythm!.kit!.kick_tune).toBe(0);
    expect(p.layers[0]).toBe(PATCH.layers[0]);
  });

  it('a state with a new pattern path fetches the pattern; get_pattern_ok fills it', () => {
    const { store, sock } = setup();
    store.getState().handle(state());
    const req = sock.sent.find((m) => (m as { type: string }).type === 'get_pattern') as { id: number } | undefined;
    expect(req).toBeDefined();
    const pat = toggleStep(emptyPattern(), 0, 0);
    sock.receive({ type: 'get_pattern_ok', id: req!.id, path: 'presets/patterns/a.json', edited: false, pattern: pat });
    expect(store.getState().pattern?.pattern.tracks[0]!.steps[0]).toBe(100);
    // same path again: no new fetch
    sock.sent = [];
    store.getState().handle(state());
    expect(sock.sent.some((m) => (m as { type: string }).type === 'get_pattern')).toBe(false);
  });

  it('step edits are optimistic and coalesced into one set_pattern per flush', () => {
    const { store, sock, runFrame } = setup();
    store.getState().handle(state());
    store.getState().handle({ type: 'get_pattern_ok', path: 'presets/patterns/a.json', edited: false, pattern: emptyPattern() });
    sock.sent = [];
    store.getState().editPattern((p) => toggleStep(p, 0, 0));
    store.getState().editPattern((p) => toggleStep(p, 1, 4));
    expect(store.getState().pattern?.edited).toBe(true);
    expect(store.getState().pattern?.pattern.tracks[1]!.steps[4]).toBe(100);
    expect(sock.sent).toHaveLength(0);
    runFrame();
    expect(sock.sent).toHaveLength(1);
    const sent = sock.sent[0] as { type: string; pattern: { tracks: { steps: number[] }[] } };
    expect(sent.type).toBe('set_pattern');
    expect(sent.pattern.tracks[0]!.steps[0]).toBe(100);
    expect(sent.pattern.tracks[1]!.steps[4]).toBe(100);
  });

  it('pattern events from other clients replace the pattern unless a local edit is pending', () => {
    const { store, runFrame } = setup();
    store.getState().handle(state());
    store.getState().handle({ type: 'get_pattern_ok', path: 'presets/patterns/a.json', edited: false, pattern: emptyPattern() });
    store.getState().handle({ type: 'pattern', path: 'presets/patterns/a.json', edited: true, pattern: toggleStep(emptyPattern(), 2, 2) });
    expect(store.getState().pattern?.pattern.tracks[2]!.steps[2]).toBe(100);
    store.getState().editPattern((p) => toggleStep(p, 3, 3));
    store.getState().handle({ type: 'pattern', path: 'presets/patterns/a.json', edited: true, pattern: emptyPattern() });
    expect(store.getState().pattern?.pattern.tracks[3]!.steps[3]).toBe(100); // local edit kept
    runFrame();
  });

  it('time signature change resizes the pattern optimistically and is sent as transport', () => {
    const { store, sock, runFrame } = setup();
    store.getState().handle(state());
    store.getState().handle({ type: 'get_pattern_ok', path: 'presets/patterns/a.json', edited: false, pattern: emptyPattern() });
    sock.sent = [];
    store.getState().setTransport({ time_sig: [7, 4] });
    expect(store.getState().pattern?.pattern.tracks[0]!.steps).toHaveLength(28);
    runFrame();
    expect(sock.sent).toEqual([{ type: 'transport', time_sig: [7, 4] }]);
  });

  it('loadPattern sends transport.pattern and drops unsent step edits', () => {
    const { store, sock, runFrame } = setup();
    store.getState().handle(state());
    store.getState().handle({ type: 'get_pattern_ok', path: 'presets/patterns/a.json', edited: false, pattern: emptyPattern() });
    sock.sent = [];
    store.getState().editPattern((p) => toggleStep(p, 0, 0));
    store.getState().loadPattern('presets/patterns/b.json');
    runFrame();
    expect(sock.sent).toEqual([{ type: 'transport', pattern: 'presets/patterns/b.json' }]);
  });
});
