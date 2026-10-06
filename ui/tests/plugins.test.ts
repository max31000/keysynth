import { describe, expect, it } from 'vitest';
import { EngineClient } from '../src/protocol/client';
import { createEngineStore } from '../src/store/engineStore';
import type { PluginStatus } from '../src/protocol/types';
import { FakeSocket, latestSocket } from './fakeSocket';

function setup() {
  FakeSocket.all = [];
  const client = new EngineClient({ url: 'ws://t', createSocket: (u) => new FakeSocket(u), jitter: 0 });
  client.connect();
  latestSocket().open();
  const store = createEngineStore({ client, schedule: () => undefined, now: () => 0 });
  return { store, sock: latestSocket() };
}

const status = (state: PluginStatus['state'], version = 1): PluginStatus => ({
  name: 'faust_pluck',
  typeId: 'plugin:faust_pluck',
  source: 'faust',
  state,
  message: state === 'error' ? 'faust_pluck.dsp:3 : ERROR : syntax error' : '',
  kind: 'instrument',
  version,
  compileMs: 40,
  cached: false,
});

describe('plugin status (docs/PROTOCOL.md plugin_status)', () => {
  it('tracks plugin_status, keeps the latest as notice, refreshes the catalog after ok/removed', () => {
    const { store, sock } = setup();
    sock.sent = [];
    store.getState().handle({ type: 'plugin_status', plugin: status('compiling', 0) });
    expect(store.getState().plugins.faust_pluck?.state).toBe('compiling');
    expect(store.getState().pluginNotice?.state).toBe('compiling');
    expect(sock.sent).toHaveLength(0);

    store.getState().handle({ type: 'plugin_status', plugin: status('error', 0) });
    expect(store.getState().pluginNotice?.message).toContain('syntax error');
    expect(sock.sent).toHaveLength(0);

    store.getState().handle({ type: 'plugin_status', plugin: status('ok', 1) });
    expect(store.getState().plugins.faust_pluck?.version).toBe(1);
    expect(sock.sent.map((m) => m.type)).toEqual(['get_catalog']);

    store.getState().handle({ type: 'plugin_status', plugin: status('removed', 1) });
    expect(store.getState().plugins.faust_pluck).toBeUndefined();
    expect(sock.sent.map((m) => m.type)).toEqual(['get_catalog', 'get_catalog']);

    store.getState().dismissPluginNotice();
    expect(store.getState().pluginNotice).toBeNull();
  });

  it('list_plugins_ok replaces the map; reloadPlugin sends the plugin name', () => {
    const { store, sock } = setup();
    store.getState().handle({
      type: 'list_plugins_ok',
      plugins: [status('ok', 3)],
      faust: { available: true, version: '2.88.0', reason: '' },
    });
    expect(Object.keys(store.getState().plugins)).toEqual(['faust_pluck']);
    sock.sent = [];
    void store.getState().reloadPlugin('faust_pluck');
    expect(sock.sent[0]).toMatchObject({ type: 'reload_plugin', name: 'faust_pluck' });
  });
});
