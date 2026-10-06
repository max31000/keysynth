import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { EngineClient } from '../src/protocol/client';
import { createEngineStore } from '../src/store/engineStore';
import { applyMoveFx, applyParam, applyZone, findModule, isAudible } from '../src/store/patchOps';
import { live } from '../src/store/liveBus';
import type { Patch, StateEvent } from '../src/protocol/types';
import { FakeSocket, latestSocket } from './fakeSocket';

const zone = { key_lo: 21, key_hi: 108, vel_lo: 1, vel_hi: 127, transpose: 0, channel: 0, volume_db: 0, pan: 0, mute: false, solo: false, sustain: true };

const PATCH: Patch = {
  format: 1,
  meta: { name: 'T' },
  layers: [
    {
      node: 1,
      name: 'A',
      zone,
      instrument: { node: 2, type: 'va', params: { cutoff: 1000 } },
      fx: [
        { node: 3, type: 'chorus', bypass: false, params: { mix: 0.5 } },
        { node: 4, type: 'reverb', bypass: false, params: { mix: 0.2 } },
        { node: 5, type: 'delay', bypass: false, params: {} },
      ],
    },
    { node: 6, name: 'B', zone: { ...zone, key_hi: 59 }, instrument: { node: 7, type: 'organ', params: {} }, fx: [] },
  ],
  master: { volume_db: -3, fx: [{ node: 8, type: 'eq', bypass: false, params: {} }] },
};

const stateMsg = (patch: Patch = PATCH): StateEvent => ({
  type: 'state',
  patch,
  presetPath: 'presets/factory/x.json',
  dirty: false,
  audio: { type: 'ASIO', name: 'X', sampleRate: 48000, bufferSize: 64, inputLatencyMs: 1, outputLatencyMs: 2, running: true },
  transport: { playing: false, tempo: 99, metronome: false, metronome_volume: 0.5 },
});

function setup() {
  FakeSocket.all = [];
  const client = new EngineClient({ url: 'ws://t', createSocket: (u) => new FakeSocket(u), jitter: 0 });
  client.connect();
  latestSocket().open();
  const queued: (() => void)[] = [];
  let t = 0;
  const store = createEngineStore({ client, schedule: (fn) => queued.push(fn), now: () => t, statsIntervalMs: 250 });
  return {
    store,
    sock: latestSocket(),
    runFrame: () => queued.splice(0).forEach((f) => f()),
    queued,
    setTime: (v: number) => (t = v),
  };
}

describe('patchOps', () => {
  it('applyParam is immutable and targets instrument, fx, master fx and master volume', () => {
    const p1 = applyParam(PATCH, 2, 'cutoff', 500);
    expect(p1).not.toBe(PATCH);
    expect(PATCH.layers[0]!.instrument.params.cutoff).toBe(1000);
    expect(p1.layers[0]!.instrument.params.cutoff).toBe(500);
    expect(p1.layers[1]).toBe(PATCH.layers[1]); // untouched layer shared
    expect(applyParam(PATCH, 4, 'mix', 0.9).layers[0]!.fx[1]!.params.mix).toBe(0.9);
    expect(applyParam(PATCH, 8, 'low_gain', 3).master.fx[0]!.params.low_gain).toBe(3);
    expect(applyParam(PATCH, 0, 'volume_db', -10).master.volume_db).toBe(-10);
    expect(applyParam(PATCH, 999, 'x', 1)).toBe(PATCH);
  });
  it('findModule', () => {
    expect(findModule(PATCH, 3)?.kind).toBe('fx');
    expect(findModule(PATCH, 7)?.layer?.name).toBe('B');
    expect(findModule(PATCH, 8)?.layer).toBeUndefined();
  });
  it('applyZone / applyMoveFx / isAudible', () => {
    expect(applyZone(PATCH, 6, { key_lo: 30 }).layers[1]!.zone).toMatchObject({ key_lo: 30, key_hi: 59 });
    expect(applyMoveFx(PATCH, 3, 2).layers[0]!.fx.map((f) => f.node)).toEqual([4, 5, 3]);
    expect(applyMoveFx(PATCH, 5, 0).layers[0]!.fx.map((f) => f.node)).toEqual([5, 3, 4]);
    const soloed = applyZone(PATCH, 6, { solo: true });
    expect(isAudible(soloed, soloed.layers[0]!)).toBe(false);
    expect(isAudible(soloed, soloed.layers[1]!)).toBe(true);
  });
});

describe('engine store', () => {
  let ctx: ReturnType<typeof setup>;
  beforeEach(() => {
    ctx = setup();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  it('state snapshot fills patch/audio/transport and picks a selection', () => {
    ctx.store.getState().handle(stateMsg());
    const s = ctx.store.getState();
    expect(s.patch?.meta.name).toBe('T');
    expect(s.transport.tempo).toBe(99);
    expect(s.audio?.bufferSize).toBe(64);
    expect(s.selectedLayer).toBe(1);
    expect(s.selectedModule).toBe(2);
  });

  it('keeps a valid selection across snapshots, repairs an invalid one', () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    st.selectModule(4, 1);
    st.handle(stateMsg());
    expect(ctx.store.getState().selectedModule).toBe(4);
    const without4 = { ...PATCH, layers: [{ ...PATCH.layers[0]!, fx: [] }, PATCH.layers[1]!] };
    st.handle(stateMsg(without4));
    expect(ctx.store.getState().selectedModule).toBe(2);
  });

  it('catalog_ok builds a type map', () => {
    ctx.store.getState().handle({ type: 'catalog_ok', modules: [{ typeId: 'va', name: 'VA', kind: 'instrument', category: 'Synth', params: [] }] });
    expect(ctx.store.getState().catalogMap.va?.name).toBe('VA');
  });

  it('setParam is optimistic and coalesced into one set_param per frame', () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    ctx.sock.sent = [];
    st.setParam(2, 'cutoff', 100);
    st.setParam(2, 'cutoff', 200);
    st.setParam(2, 'cutoff', 300);
    st.setParam(3, 'mix', 0.1);
    expect(findModule(ctx.store.getState().patch, 2)?.slot.params.cutoff).toBe(300);
    expect(ctx.store.getState().dirty).toBe(true);
    expect(ctx.sock.sent).toHaveLength(0);
    expect(ctx.queued).toHaveLength(1);
    ctx.runFrame();
    expect(ctx.sock.sent).toEqual([
      { type: 'set_param', node: 2, param: 'cutoff', value: 300 },
      { type: 'set_param', node: 3, param: 'mix', value: 0.1 },
    ]);
  });

  it('setZone merges partial zones per layer', () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    ctx.sock.sent = [];
    st.setZone(1, { key_lo: 40 });
    st.setZone(1, { key_hi: 70 });
    ctx.runFrame();
    expect(ctx.sock.sent).toEqual([{ type: 'set_zone', layer: 1, zone: { key_lo: 40, key_hi: 70 } }]);
  });

  it('unsent optimistic edits survive an incoming snapshot', () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    st.setParam(2, 'cutoff', 4321);
    st.handle(stateMsg()); // engine snapshot still has 1000
    expect(findModule(ctx.store.getState().patch, 2)?.slot.params.cutoff).toBe(4321);
  });

  it('param event from another client updates the patch', () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    st.handle({ type: 'param', node: 4, param: 'mix', value: 0.77 });
    expect(findModule(ctx.store.getState().patch, 4)?.slot.params.mix).toBe(0.77);
  });

  it('telemetry goes to the live bus; stats are throttled', () => {
    const st = ctx.store.getState();
    const t = (cpu: number) => ({ type: 'telemetry' as const, cpu, xruns: 0, voices: 2, meters: { master: [-6, -6] as [number, number], layers: {} }, readouts: {} });
    ctx.setTime(1000);
    st.handle(t(0.1));
    expect(ctx.store.getState().stats.cpu).toBe(0.1);
    ctx.setTime(1100);
    st.handle(t(0.2));
    expect(live.telemetry?.cpu).toBe(0.2);
    expect(ctx.store.getState().stats.cpu).toBe(0.1);
    ctx.setTime(1300);
    st.handle(t(0.3));
    expect(ctx.store.getState().stats.cpu).toBe(0.3);
  });

  it('midi events update active notes', () => {
    const st = ctx.store.getState();
    st.handle({ type: 'midi', notes: [{ note: 60, on: true, velocity: 99 }, { note: 64, on: true, velocity: 80 }] });
    expect(live.engineNotes[60]).toBe(99);
    st.handle({ type: 'midi', notes: [{ note: 60, on: false, velocity: 0 }] });
    expect(live.engineNotes[60]).toBe(0);
    expect(live.engineNotes[64]).toBe(80);
  });

  it('bypass and transport are optimistic and queued like params', () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    ctx.sock.sent = [];
    st.setBypass(3, true);
    st.setTransport({ tempo: 130 });
    st.setTransport({ tempo: 140 });
    expect(findModule(ctx.store.getState().patch, 3)?.slot).toMatchObject({ bypass: true });
    expect(ctx.store.getState().transport.tempo).toBe(140);
    expect(ctx.sock.sent).toHaveLength(0);
    ctx.runFrame();
    expect(ctx.sock.sent).toEqual([
      { type: 'set_fx_bypass', node: 3, bypass: true },
      { type: 'transport', tempo: 140 },
    ]);
  });

  it('structural ops use the documented wire fields', async () => {
    const st = ctx.store.getState();
    st.handle(stateMsg());
    ctx.sock.sent = [];
    void st.setInstrument(1, 'organ');
    void st.addFx(0, 'reverb');
    void st.setAudioDevice('ASIO', 'Dev', 96000, 32);
    void st.restartAudio();
    expect(ctx.sock.sent[0]).toMatchObject({ type: 'set_instrument', layer: 1, module: 'organ' });
    expect(ctx.sock.sent[1]).toMatchObject({ type: 'add_fx', layer: 0, module: 'reverb' });
    expect(ctx.sock.sent[2]).toMatchObject({ type: 'set_audio_device', device_type: 'ASIO', name: 'Dev', sample_rate: 96000, buffer_size: 32 });
    expect(ctx.sock.sent[3]).toMatchObject({ type: 'restart_audio' });
  });

  it('offline edits stay pending and are re-sent after reconnect + state; stale targets are pruned', () => {
    vi.useFakeTimers();
    ctx = setup();
    const st = ctx.store.getState();
    st.handle(stateMsg());
    // go offline
    ctx.sock.drop();
    st.connectionChanged('reconnecting');
    st.setParam(2, 'cutoff', 777);
    st.setParam(4, 'mix', 0.9); // reverb, will be gone after reconnect
    st.setZone(1, { transpose: 12 });
    st.setBypass(3, true);
    st.setTransport({ tempo: 150 });
    ctx.runFrame(); // flush while offline → nothing lost
    expect(findModule(ctx.store.getState().patch, 2)?.slot.params.cutoff).toBe(777);
    // reconnect: the client opens a new socket after backoff
    vi.advanceTimersByTime(1000);
    const sock2 = latestSocket();
    expect(sock2).not.toBe(ctx.sock);
    sock2.open();
    st.connectionChanged('open');
    // engine snapshot no longer has node 4, and node 3 now holds a different module type
    const snap: Patch = {
      ...PATCH,
      layers: [
        { ...PATCH.layers[0]!, fx: [{ node: 3, type: 'phaser', bypass: false, params: {} }] },
        PATCH.layers[1]!,
      ],
    };
    st.handle(stateMsg(snap));
    // pending edits shown on top of the snapshot
    const s = ctx.store.getState();
    expect(findModule(s.patch, 2)?.slot.params.cutoff).toBe(777);
    expect(s.patch!.layers[0]!.zone.transpose).toBe(12);
    expect(s.transport.tempo).toBe(150);
    expect(s.dirty).toBe(true);
    ctx.runFrame();
    expect(sock2.sent).toEqual([
      { type: 'set_param', node: 2, param: 'cutoff', value: 777 },
      { type: 'set_zone', layer: 1, zone: { transpose: 12 } },
      { type: 'transport', tempo: 150 },
    ]);
    // nothing left over
    sock2.sent = [];
    ctx.runFrame();
    expect(sock2.sent).toEqual([]);
    vi.useRealTimers();
  });

  it('nothing is sent before the first state of a connection', () => {
    const st = ctx.store.getState();
    st.setParam(2, 'cutoff', 5);
    ctx.runFrame();
    expect(ctx.sock.sent).toEqual([]);
    st.handle(stateMsg());
    ctx.runFrame();
    expect(ctx.sock.sent).toEqual([{ type: 'set_param', node: 2, param: 'cutoff', value: 5 }]);
  });

  it('request errors surface as lastError', async () => {
    const st = ctx.store.getState();
    const p = st.loadPreset('nope');
    const sent = ctx.sock.last() as { id: number };
    ctx.sock.receive({ type: 'error', id: sent.id, code: 'not_found', message: 'preset not found' });
    await p;
    expect(ctx.store.getState().lastError).toBe('preset not found');
  });

  it('only log events flagged notify become a toast; all are kept in the log', () => {
    const st = ctx.store.getState();
    st.handle({ type: 'log', level: 'warn', message: "module 'plugin:x' unavailable for node 3" });
    st.handle({ type: 'log', level: 'error', message: 'plugin x: compile error' });
    expect(ctx.store.getState().lastError).toBeNull();
    st.handle({ type: 'log', level: 'warn', notify: true, message: 'the driver did not accept a buffer of 32 samples' });
    expect(ctx.store.getState().lastError).toBe('the driver did not accept a buffer of 32 samples');
    expect(ctx.store.getState().logs.map((l) => l.level)).toEqual(['warn', 'error', 'warn']);
  });
});
