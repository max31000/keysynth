// MIDI processors (layer.midi, ARCHITECTURE §17): patchOps find / set_param echo / bypass / move by node, store
// actions (add_fx, remove_fx, chord_learn), chord helpers.
import { describe, expect, it } from 'vitest';
import { EngineClient } from '../src/protocol/client';
import { createEngineStore } from '../src/store/engineStore';
import { applyBypass, applyMoveFx, applyParam, findModule } from '../src/store/patchOps';
import { heldNotes, intervalNames, memoryIntervals } from '../src/lib/chord';
import type { Patch } from '../src/protocol/types';
import { FakeSocket, latestSocket } from './fakeSocket';

const zone = { key_lo: 21, key_hi: 108, vel_lo: 1, vel_hi: 127, transpose: 0, channel: 0, volume_db: 0, pan: 0, mute: false, solo: false, sustain: true };
const PATCH: Patch = {
  format: 2,
  meta: { name: 'T' },
  layers: [
    {
      node: 1,
      name: 'A',
      zone,
      instrument: { node: 2, type: 'va', params: {} },
      fx: [{ node: 3, type: 'delay', bypass: false, params: { mix: 0.2 } }],
      midi: [
        { node: 4, type: 'arp', bypass: false, params: { mode: 0 } },
        { node: 5, type: 'chord', bypass: false, params: { type: 0, mem_count: 0 } },
      ],
    },
    { node: 6, name: 'B', zone, instrument: { node: 7, type: 'organ', params: {} }, fx: [] },
  ],
  master: { volume_db: 0, fx: [] },
};

describe('patchOps with layer.midi', () => {
  it('findModule finds MIDI slots by node (kind midi, owning layer)', () => {
    const m = findModule(PATCH, 5);
    expect(m?.kind).toBe('midi');
    expect(m?.slot.type).toBe('chord');
    expect(m?.layer?.node).toBe(1);
    expect(findModule(PATCH, 3)?.kind).toBe('fx');
  });

  it('applyParam (set_param echo) and applyBypass target MIDI slots immutably', () => {
    const p = applyParam(PATCH, 4, 'mode', 2);
    expect(p.layers[0]!.midi![0]!.params.mode).toBe(2);
    expect(PATCH.layers[0]!.midi![0]!.params.mode).toBe(0);
    expect(p.layers[0]!.fx).toBe(PATCH.layers[0]!.fx);
    expect(p.layers[1]).toBe(PATCH.layers[1]);
    const b = applyBypass(PATCH, 5, true);
    expect(b.layers[0]!.midi![1]!.bypass).toBe(true);
    expect(applyParam(PATCH, 99, 'x', 1)).toBe(PATCH);
  });

  it('applyMoveFx reorders within the MIDI chain only', () => {
    const p = applyMoveFx(PATCH, 5, 0);
    expect(p.layers[0]!.midi!.map((s) => s.node)).toEqual([5, 4]);
    expect(p.layers[0]!.fx).toBe(PATCH.layers[0]!.fx);
    // a layer without a MIDI chain stays as it is
    expect(applyMoveFx(PATCH, 3, 0).layers[1]).toBe(PATCH.layers[1]);
  });
});

describe('chord helpers', () => {
  it('memory from hidden params, names over C, held readout', () => {
    expect(memoryIntervals({ mem_count: 3, mem_1: 0, mem_2: 4, mem_3: 7, mem_4: 11 })).toEqual([0, 4, 7]);
    expect(memoryIntervals({ mem_count: 0 })).toEqual([]);
    expect(memoryIntervals(undefined)).toEqual([]);
    expect(intervalNames([0, 4, 7, 10, 14])).toBe('C E G A# D');
    expect(heldNotes({ held_count: 2, held_1: 60, held_2: 64, held_3: -1 })).toEqual([60, 64]);
    expect(heldNotes(undefined)).toEqual([]);
  });
});

describe('store: MIDI chain actions', () => {
  function setup() {
    FakeSocket.all = [];
    const client = new EngineClient({ url: 'ws://t', createSocket: (u) => new FakeSocket(u), jitter: 0 });
    client.connect();
    latestSocket().open();
    const queued: (() => void)[] = [];
    const store = createEngineStore({ client, schedule: (fn) => queued.push(fn) });
    client.on('*', (m) => store.getState().handle(m));
    const sock = latestSocket();
    sock.receive({
      type: 'state',
      patch: PATCH,
      presetPath: null,
      dirty: false,
      audio: {},
      transport: { playing: false, tempo: 120, metronome: false, metronome_volume: 0.5 },
    });
    return { store, sock, runFrame: () => queued.splice(0).forEach((f) => f()) };
  }

  it('param event and bypass on a MIDI slot', () => {
    const { store, sock, runFrame } = setup();
    sock.receive({ type: 'param', node: 5, param: 'mem_count', value: 3 });
    expect(findModule(store.getState().patch, 5)?.slot.params.mem_count).toBe(3);
    store.getState().setBypass(4, true);
    expect((findModule(store.getState().patch, 4)?.slot as { bypass?: boolean }).bypass).toBe(true);
    runFrame();
    expect(sock.last()).toEqual({ type: 'set_fx_bypass', node: 4, bypass: true });
  });

  it('a pending edit on a MIDI slot survives a state snapshot that still has it', () => {
    const { store, sock } = setup();
    store.getState().setParam(4, 'mode', 3);
    sock.receive({ type: 'state', patch: PATCH, presetPath: null, dirty: false, audio: {}, transport: {} });
    expect(findModule(store.getState().patch, 4)?.slot.params.mode).toBe(3);
  });

  it('add / remove / move a MIDI processor use add_fx / remove_fx / move_fx', async () => {
    const { store, sock } = setup();
    const reply = () => sock.receive({ type: 'state', id: sock.last().id, patch: PATCH, presetPath: null, dirty: true, audio: {}, transport: {} });
    let p = store.getState().addFx(1, 'hold');
    expect(sock.last()).toMatchObject({ type: 'add_fx', layer: 1, module: 'hold' });
    reply();
    await p;
    p = store.getState().removeFx(4);
    expect(sock.last()).toMatchObject({ type: 'remove_fx', node: 4 });
    reply();
    await p;
    p = store.getState().moveFx(5, 0);
    expect(store.getState().patch!.layers[0]!.midi!.map((s) => s.node)).toEqual([5, 4]); // optimistic
    expect(sock.last()).toMatchObject({ type: 'move_fx', node: 5, to: 0 });
    reply();
    await p;
  });

  it('chordLearn resolves to null or the error code', async () => {
    const { store, sock } = setup();
    let p = store.getState().chordLearn(5);
    expect(sock.last()).toMatchObject({ type: 'chord_learn', node: 5 });
    sock.receive({ type: 'error', id: sock.last().id, code: 'no_notes', message: 'hold 2 or more notes' });
    expect(await p).toBe('no_notes');
    p = store.getState().chordLearn(5);
    sock.receive({ type: 'chord_learn_ok', id: sock.last().id, node: 5, intervals: [0, 4, 7] });
    expect(await p).toBeNull();
    expect(store.getState().lastError).toBeNull(); // a learn error is shown by the panel, not as a toast
  });
});
