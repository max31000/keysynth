// Mock relay hub for the controller UI (docs/specs/2026-10-12-remote.md §10 envelope), port 7442:
//   ws://127.0.0.1:7442/keysynth/api/relay?role=controller&cid=<id>   (no auth; dev only)
// Every mock host bridges to a mock engine (default ws://127.0.0.1:7441, `npm run mock`) with one engine
// connection per (controller, host), marked as a relay session (trust `remote`, RELAY_HEADER). Hosts can be
// switched off/on to exercise reconnects:
//   GET http://127.0.0.1:7442/__relay/state
//   GET http://127.0.0.1:7442/__relay/online?host=mock-2&v=0|1
//
//   npm run relay-mock                    (1 host "Mock PC"; `-- --hosts 2` adds "Mock PC 2")
//   npm run relay-mock -- --own-engine    (starts its own mock engine in-process)
// Engine relay-host E2E (real engine as a host) uses scripts/dev_relay_server.py instead.

import { createServer, type IncomingMessage } from 'node:http';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { RELAY_HEADER, startMockEngine, type MockEngine } from './mock-engine';

export const RELAY_MOCK_PORT = 7442;
export const RELAY_PATH = '/keysynth/api/relay';

export interface RelayMockOptions {
  port?: number;
  host?: string;
  /** mock engine every host bridges to */
  engineUrl: string;
  /** number of mock hosts (default 1) */
  hosts?: number;
  /** server ping interval (default 20 s) */
  pingMs?: number;
  quiet?: boolean;
}

export interface RelayMock {
  port: number;
  setOnline(deviceId: string, online: boolean): void;
  /** frames received from controllers, by `t` (tests) */
  readonly stats: Record<string, number>;
  /** last `{t:"msg"}` payloads per controller (tests) */
  readonly lastMsgs: Record<string, unknown>[];
  close(): Promise<void>;
}

interface MockHost {
  deviceId: string;
  name: string;
  version: string;
  online: boolean;
}

interface Controller {
  cid: string;
  ws: WebSocket;
  paused: boolean;
  /** engine connections per host */
  bridges: Map<string, Bridge>;
}

interface Bridge {
  ws: WebSocket;
  queue: string[];
}

export function startRelayMock(opts: RelayMockOptions): Promise<RelayMock> {
  const log = (...a: unknown[]) => {
    if (!opts.quiet) console.log('[relay-mock]', ...a);
  };
  const hosts: MockHost[] = Array.from({ length: Math.max(1, opts.hosts ?? 1) }, (_, i) => ({
    deviceId: `mock-${i + 1}`,
    name: i === 0 ? 'Mock PC' : `Mock PC ${i + 1}`,
    version: '0.1.0',
    online: true,
  }));
  const controllers = new Map<string, Controller>();
  const stats: Record<string, number> = {};
  const lastMsgs: Record<string, unknown>[] = [];

  const hostsMsg = () => ({ t: 'hosts', hosts: hosts.map((h) => ({ ...h })) });
  const send = (ws: WebSocket, obj: object) => {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  };
  const broadcastHosts = () => {
    for (const c of controllers.values()) send(c.ws, hostsMsg());
  };

  /** engine connection of (controller, host); messages sent before it opens are queued */
  const bridgeFor = (c: Controller, h: MockHost): Bridge => {
    const cur = c.bridges.get(h.deviceId);
    if (cur && cur.ws.readyState <= WebSocket.OPEN) return cur;
    const b: Bridge = { ws: new WebSocket(opts.engineUrl, { headers: { [RELAY_HEADER]: '1' } }), queue: [] };
    b.ws.on('open', () => {
      for (const q of b.queue.splice(0)) b.ws.send(q);
    });
    b.ws.on('message', (raw) => {
      let msg: unknown;
      try {
        msg = JSON.parse(String(raw));
      } catch {
        return;
      }
      const t = (msg as { type?: string }).type;
      const ctl = controllers.get(c.cid);
      if (!ctl || !h.online) return;
      if (ctl.paused && (t === 'telemetry' || t === 'midi')) return; // host skips telemetry for paused controllers
      send(ctl.ws, { t: 'msg', host: h.deviceId, msg });
    });
    b.ws.on('error', () => undefined);
    c.bridges.set(h.deviceId, b);
    return b;
  };
  const toBridge = (b: Bridge, text: string) => {
    if (b.ws.readyState === WebSocket.OPEN) b.ws.send(text);
    else b.queue.push(text);
  };
  const dropBridge = (c: Controller, deviceId: string) => {
    c.bridges.get(deviceId)?.ws.terminate();
    c.bridges.delete(deviceId);
  };

  const http = createServer((req, res) => {
    const url = new URL(req.url ?? '/', 'http://relay');
    res.setHeader('Content-Type', 'application/json');
    res.setHeader('Access-Control-Allow-Origin', '*');
    if (url.pathname === '/__relay/state') {
      return res.end(JSON.stringify({ hosts, controllers: [...controllers.values()].map((c) => ({ cid: c.cid, paused: c.paused })), stats }));
    }
    if (url.pathname === '/__relay/online') {
      const h = hosts.find((x) => x.deviceId === url.searchParams.get('host'));
      if (!h) {
        res.statusCode = 404;
        return res.end(JSON.stringify({ error: 'not_found' }));
      }
      api.setOnline(h.deviceId, url.searchParams.get('v') !== '0');
      return res.end(JSON.stringify(h));
    }
    res.statusCode = 404;
    res.end(JSON.stringify({ error: 'not_found' }));
  });

  const wss = new WebSocketServer({ noServer: true });
  http.on('upgrade', (req: IncomingMessage, socket, head) => {
    const url = new URL(req.url ?? '/', 'http://relay');
    if (url.pathname !== RELAY_PATH || url.searchParams.get('role') !== 'controller') {
      socket.write('HTTP/1.1 400 Bad Request\r\n\r\n');
      socket.destroy();
      return;
    }
    const cid = url.searchParams.get('cid') || randomUUID();
    wss.handleUpgrade(req, socket, head, (ws) => onController(ws, cid));
  });

  const onController = (ws: WebSocket, cid: string) => {
    const old = controllers.get(cid);
    // resume (same cid, e.g. a renewal): the new socket takes over the bridges, the old one is closed
    const c: Controller = { cid, ws, paused: old?.paused ?? false, bridges: old?.bridges ?? new Map() };
    controllers.set(cid, c);
    if (old) old.ws.close(4000, 'resumed elsewhere');
    log(old ? 'controller resumed' : 'controller joined', cid);
    send(ws, hostsMsg());
    let lastRx = Date.now();
    ws.on('message', (raw) => {
      lastRx = Date.now();
      let m: Record<string, unknown>;
      try {
        m = JSON.parse(String(raw)) as Record<string, unknown>;
      } catch {
        return;
      }
      const t = String(m.t);
      stats[t] = (stats[t] ?? 0) + 1;
      if (t === 'ping') return send(ws, { t: 'pong' });
      if (t === 'pause' || t === 'resume') {
        c.paused = t === 'pause';
        return;
      }
      if (t !== 'msg') return;
      const h = hosts.find((x) => x.deviceId === m.host);
      if (!h || !h.online) {
        const id = (m.msg as { id?: unknown } | undefined)?.id;
        return send(ws, { t: 'error', code: 'host_offline', host: m.host, ...(typeof id === 'number' ? { id } : {}) });
      }
      lastMsgs.push(m.msg as Record<string, unknown>);
      if (lastMsgs.length > 200) lastMsgs.shift();
      toBridge(bridgeFor(controllers.get(cid) ?? c, h), JSON.stringify(m.msg));
    });
    const pinger = setInterval(() => {
      if (Date.now() - lastRx > 45_000) ws.close(4001, 'silence');
      else send(ws, { t: 'ping' });
    }, opts.pingMs ?? 20_000);
    ws.on('close', () => {
      clearInterval(pinger);
      if (controllers.get(cid) !== c) return; // replaced by a resumed socket
      controllers.delete(cid);
      for (const id of [...c.bridges.keys()]) dropBridge(c, id);
      log('controller left', cid);
    });
  };

  const api: RelayMock = {
    port: 0,
    stats,
    lastMsgs,
    setOnline(deviceId, online) {
      const h = hosts.find((x) => x.deviceId === deviceId);
      if (!h || h.online === online) return;
      h.online = online;
      // an offline host loses its sessions (the engine forgets the virtual clients)
      if (!online) for (const c of controllers.values()) dropBridge(c, deviceId);
      log(`host ${deviceId} ${online ? 'online' : 'offline'}`);
      broadcastHosts();
    },
    close: () =>
      new Promise<void>((resolve) => {
        for (const c of controllers.values()) {
          for (const id of [...c.bridges.keys()]) dropBridge(c, id);
          c.ws.terminate();
        }
        controllers.clear();
        wss.close();
        http.close(() => resolve());
      }),
  };

  return new Promise((resolve, reject) => {
    http.once('error', reject);
    http.listen(opts.port ?? RELAY_MOCK_PORT, opts.host ?? '127.0.0.1', () => {
      const addr = http.address();
      api.port = typeof addr === 'object' && addr ? addr.port : (opts.port ?? RELAY_MOCK_PORT);
      log(`listening on ws://127.0.0.1:${api.port}${RELAY_PATH} → ${opts.engineUrl} (${hosts.length} host${hosts.length > 1 ? 's' : ''})`);
      resolve(api);
    });
  });
}

// CLI
const isMain =
  !!process.argv[1] && path.resolve(fileURLToPath(import.meta.url)).toLowerCase() === path.resolve(process.argv[1]).toLowerCase();
if (isMain) {
  const args = process.argv.slice(2);
  const val = (k: string) => {
    const i = args.indexOf(k);
    return i >= 0 ? args[i + 1] : undefined;
  };
  void (async () => {
    let own: MockEngine | null = null;
    if (args.includes('--own-engine')) own = await startMockEngine({ port: 0 });
    await startRelayMock({
      port: Number(val('--port') ?? RELAY_MOCK_PORT),
      engineUrl: own ? `ws://127.0.0.1:${own.port}` : (val('--engine') ?? 'ws://127.0.0.1:7441'),
      hosts: Number(val('--hosts') ?? 1),
    });
  })().catch((e: unknown) => {
    console.error('[relay-mock] failed to start:', e);
    process.exit(1);
  });
}
