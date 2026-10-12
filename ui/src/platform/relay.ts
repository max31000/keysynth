// Controller side of the relay (docs/specs/2026-10-12-remote.md, §10 wins): one WebSocket to
// `<origin>/keysynth/api/relay?role=controller&cid=<uuid>` (platform cookie), the hosts list, host selection and a
// `WebSocketLike` adapter (RelayTransport) that EngineClient uses instead of a direct engine socket, so the
// stores and panels work unchanged.
//
// Envelope: controller → server `{t:"msg", host, msg}`, `{t:"pause"}`, `{t:"resume"}`, `{t:"ping"}`/`{t:"pong"}`;
// server → controller `{t:"hosts", hosts}`, `{t:"msg", host, msg}`, `{t:"error", code}`, `{t:"ping"}`/`{t:"pong"}`.
//
// - RelayTransport is CONNECTING until the relay socket is open, the hosts list arrived and the selected host is
//   online; it closes when the relay socket drops, the host goes offline or another host is selected. EngineClient
//   then rejects pending requests (`disconnected`), reconnects and sends `hello` again (= resync, §10.7).
// - Replies resolve requests by `id` only (`strictReplies`): no id-less fallback through the relay.
// - `set_param` is coalesced to ≤ 30 Hz per transport (latest value per node/param wins; flushed before any other
//   message so the order of edits is kept).
// - Heartbeat: `{t:"ping"}` every 20 s (RTT from the `pong`), answer server pings, reconnect after 45 s silence.
// - Renewal (§10.2): ~50 min after a socket opened, refresh the session (getMe) and open a second socket with the
//   same `cid`; once it delivers `hosts` it replaces the old one (make-before-break; the transport stays open).
// - Hidden tab → `{t:"pause"}` (host skips telemetry), visible → `{t:"resume"}`, and a dead socket reconnects
//   at once instead of waiting for the backoff (iPad Safari freezes background tabs).

import type { WebSocketFactory, WebSocketLike } from '../protocol/client';
import type { StorageLike } from './discovery';

export interface RelayHost {
  deviceId: string;
  name: string;
  online: boolean;
  version?: string;
}

export type RelayStatus = 'idle' | 'connecting' | 'open' | 'reconnecting';

export interface RelayState {
  status: RelayStatus;
  /** last `hosts` list (kept while reconnecting) */
  hosts: RelayHost[];
  /** host the transport talks to; null = none online / several online and none chosen yet */
  hostId: string | null;
  /** browser ⇄ relay round trip of the last ping, ms */
  rtt: number | null;
  /** last `{t:"error"}` code from the server */
  error: string | null;
  /** failed connection attempts since the last open socket */
  failures: number;
}

export const LAST_HOST_KEY = 'keysynth.relayHost';

/** One online → it; the current one while it stays online; else the last chosen one if online; else none. */
export function pickHost(hosts: RelayHost[], current: string | null, lastChosen: string | null): string | null {
  const online = hosts.filter((h) => h.online);
  if (current && online.some((h) => h.deviceId === current)) return current;
  if (online.length === 1) return online[0]!.deviceId;
  if (lastChosen && online.some((h) => h.deviceId === lastChosen)) return lastChosen;
  return null;
}

/** `wss://<host>/keysynth/api/relay` for the page's origin (`ws://` on plain http dev servers). */
export function relayUrlFor(loc: { protocol: string; host: string }, apiBase: string, override?: string): string {
  if (override) return override;
  return `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}${apiBase}/relay`;
}

export function parseHosts(v: unknown): RelayHost[] {
  if (!Array.isArray(v)) return [];
  const out: RelayHost[] = [];
  for (const h of v) {
    if (!h || typeof h !== 'object') continue;
    const o = h as Record<string, unknown>;
    if (typeof o.deviceId !== 'string' || !o.deviceId) continue;
    const host: RelayHost = {
      deviceId: o.deviceId,
      name: typeof o.name === 'string' && o.name ? o.name : o.deviceId.slice(0, 8),
      online: o.online !== false,
    };
    if (typeof o.version === 'string') host.version = o.version;
    out.push(host);
  }
  return out;
}

export interface RelayOptions {
  /** relay endpoint without query (relayUrlFor) */
  url: string;
  /** controller id (client-generated UUID); reused by renewals */
  cid?: string;
  createSocket?: WebSocketFactory;
  /** refresh the platform session cookie (kit `getMe`); before reconnects and renewals */
  refresh?: () => Promise<unknown>;
  storage?: StorageLike | null;
  now?: () => number;
  pingMs?: number;
  silenceMs?: number;
  renewMs?: number;
  backoffInitialMs?: number;
  backoffMaxMs?: number;
  /** 0..1 fraction of the delay randomised; 0 in tests */
  jitter?: number;
  /** min interval between `set_param` flushes, ms (30 Hz) */
  paramIntervalMs?: number;
}

const WS_OPEN = 1;
const HEARTBEAT_TICK_MS = 5000;
const RENEW_RETRY_MS = 60_000;

function uuid(): string {
  const c = (globalThis as { crypto?: { randomUUID?: () => string } }).crypto;
  if (c?.randomUUID) return c.randomUUID();
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (ch) => {
    const r = (Math.random() * 16) | 0;
    return (ch === 'x' ? r : (r & 3) | 8).toString(16);
  });
}

export class RelayLink {
  readonly cid: string;
  private readonly o: Required<Omit<RelayOptions, 'cid' | 'storage' | 'refresh' | 'createSocket'>>;
  private readonly createSocket: WebSocketFactory;
  private readonly refresh: () => Promise<unknown>;
  private readonly storage: StorageLike | null;
  private ws: WebSocketLike | null = null;
  /** renewal socket (same cid) until it delivers `hosts` */
  private renewing: WebSocketLike | null = null;
  /** the current socket delivered `hosts` */
  private ready = false;
  private wantOpen = false;
  private attempt = 0;
  private visible = true;
  private lastRx = 0;
  private lastPing = 0;
  private pingSentAt: number | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private heartbeat: ReturnType<typeof setInterval> | null = null;
  private renewTimer: ReturnType<typeof setTimeout> | null = null;
  private transport_: RelayTransport | null = null;
  private state: RelayState = { status: 'idle', hosts: [], hostId: null, rtt: null, error: null, failures: 0 };
  private listeners = new Set<() => void>();

  constructor(opts: RelayOptions) {
    this.cid = opts.cid ?? uuid();
    this.o = {
      url: opts.url,
      now: opts.now ?? (() => Date.now()),
      pingMs: opts.pingMs ?? 20_000,
      silenceMs: opts.silenceMs ?? 45_000,
      renewMs: opts.renewMs ?? 50 * 60_000,
      backoffInitialMs: opts.backoffInitialMs ?? 1000,
      backoffMaxMs: opts.backoffMaxMs ?? 30_000,
      jitter: opts.jitter ?? 0.2,
      paramIntervalMs: opts.paramIntervalMs ?? 33,
    };
    this.createSocket = opts.createSocket ?? ((u) => new WebSocket(u) as unknown as WebSocketLike);
    this.refresh = opts.refresh ?? (() => Promise.resolve());
    this.storage = opts.storage ?? null;
  }

  // ─────────────── public API ───────────────

  getState = (): RelayState => this.state;

  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  get socketUrl(): string {
    return `${this.o.url}${this.o.url.includes('?') ? '&' : '?'}role=controller&cid=${encodeURIComponent(this.cid)}`;
  }

  start(): void {
    this.wantOpen = true;
    if (this.ws || this.reconnectTimer) return;
    this.open();
  }

  stop(): void {
    this.wantOpen = false;
    this.clearTimers();
    this.retire(this.renewing);
    this.renewing = null;
    this.retire(this.ws);
    this.ws = null;
    this.ready = false;
    this.dropTransport();
    this.attempt = 0;
    this.set({ status: 'idle', hosts: [], hostId: null, rtt: null, error: null, failures: 0 });
  }

  /** Skip a pending backoff delay. */
  retryNow(): void {
    if (!this.wantOpen || this.ws) return;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    this.open();
  }

  /** User picked a host in the switcher (remembered for the next visit). */
  selectHost(deviceId: string): void {
    this.remember(deviceId);
    if (this.state.hostId === deviceId) return;
    this.set({ hostId: deviceId });
    this.syncTransport();
  }

  /** Page visibility (document.visibilityState === 'visible'). */
  setVisible(visible: boolean): void {
    if (visible === this.visible) return;
    this.visible = visible;
    if (this.ws && this.ws.readyState === WS_OPEN) this.sendFrame({ t: visible ? 'resume' : 'pause' });
    if (!visible) return;
    if (!this.ws) this.retryNow();
    // a frozen tab's socket may look open but be dead: check silence now and probe with a ping
    else if (this.ws.readyState === WS_OPEN) {
      if (this.o.now() - this.lastRx > this.o.silenceMs) this.socketLost(this.ws);
      else this.ping();
    }
  }

  /** WebSocketLike for EngineClient (`createSocket` while the route is the relay). */
  transport(): WebSocketLike {
    this.dropTransport();
    const t = new RelayTransport(this, this.o.paramIntervalMs, this.o.now);
    this.transport_ = t;
    // async like a real socket: EngineClient assigns its handlers right after the factory returns
    queueMicrotask(() => this.syncTransport());
    return t;
  }

  // ─────────────── used by RelayTransport ───────────────

  /** @internal */
  sendMsg(t: RelayTransport, msg: unknown): void {
    if (t !== this.transport_ || !t.host) return;
    this.sendFrame({ t: 'msg', host: t.host, msg });
  }

  /** @internal */
  detach(t: RelayTransport): void {
    if (this.transport_ === t) this.transport_ = null;
  }

  // ─────────────── internals ───────────────

  private set(p: Partial<RelayState>): void {
    this.state = { ...this.state, ...p };
    for (const l of [...this.listeners]) l();
  }

  private remember(id: string): void {
    try {
      this.storage?.setItem(LAST_HOST_KEY, id);
    } catch {
      /* private mode */
    }
  }

  private lastChosen(): string | null {
    try {
      return this.storage?.getItem(LAST_HOST_KEY) ?? null;
    } catch {
      return null;
    }
  }

  private sendFrame(obj: object, ws = this.ws): boolean {
    if (!ws || ws.readyState !== WS_OPEN) return false;
    ws.send(JSON.stringify(obj));
    return true;
  }

  private open(): void {
    this.set({ status: this.attempt === 0 ? 'connecting' : 'reconnecting' });
    const ws = this.connectSocket();
    if (!ws) {
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    this.ready = false;
  }

  private connectSocket(): WebSocketLike | null {
    let ws: WebSocketLike;
    try {
      ws = this.createSocket(this.socketUrl);
    } catch {
      return null;
    }
    ws.onopen = () => this.onOpen(ws);
    ws.onmessage = (ev) => this.onFrame(ws, ev.data);
    ws.onerror = () => {
      /* onclose follows */
    };
    ws.onclose = () => this.socketLost(ws);
    return ws;
  }

  private retire(ws: WebSocketLike | null): void {
    if (!ws) return;
    ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;
    try {
      ws.close(1000);
    } catch {
      /* already closed */
    }
  }

  private onOpen(ws: WebSocketLike): void {
    if (ws !== this.ws) return; // a renewal socket counts once it delivers `hosts`
    this.lastRx = this.o.now();
    this.startTimers();
    if (!this.visible) this.sendFrame({ t: 'pause' });
    this.ping();
  }

  private onFrame(ws: WebSocketLike, data: unknown): void {
    if (ws !== this.ws && ws !== this.renewing) return;
    if (typeof data !== 'string') return;
    let m: Record<string, unknown>;
    try {
      m = JSON.parse(data) as Record<string, unknown>;
    } catch {
      return;
    }
    if (!m || typeof m !== 'object') return;
    if (ws === this.ws) this.lastRx = this.o.now();
    switch (m.t) {
      case 'hosts':
        if (ws === this.renewing) this.promote();
        this.onHosts(parseHosts(m.hosts));
        break;
      case 'msg': {
        const t = this.transport_;
        if (t && t.readyState === WS_OPEN && m.host === t.host && m.msg && typeof m.msg === 'object')
          t.deliver(JSON.stringify(m.msg));
        break;
      }
      case 'ping':
        this.sendFrame({ t: 'pong' }, ws);
        break;
      case 'pong':
        if (ws === this.ws && this.pingSentAt !== null) {
          this.set({ rtt: Math.max(0, this.o.now() - this.pingSentAt) });
          this.pingSentAt = null;
        }
        break;
      case 'error': {
        const code = typeof m.code === 'string' ? m.code : 'error';
        this.set({ error: code });
        // `host_offline` echoes the inner request's id: fail that request now instead of at its timeout
        const t = this.transport_;
        if (t && t.readyState === WS_OPEN && typeof m.id === 'number' && (m.host === undefined || m.host === t.host))
          t.deliver(JSON.stringify({ type: 'error', id: m.id, code, message: code === 'host_offline' ? 'engine offline' : code }));
        break;
      }
      default:
        break;
    }
  }

  private onHosts(hosts: RelayHost[]): void {
    const first = !this.ready;
    this.ready = true;
    if (first) {
      this.attempt = 0;
      this.set({ status: 'open', failures: 0, error: null });
    }
    const hostId = pickHost(hosts, this.state.hostId, this.lastChosen());
    if (hostId && hostId !== this.state.hostId) this.remember(hostId);
    this.set({ hosts, hostId });
    this.syncTransport();
  }

  /** Open / close the transport for the current socket + host state. */
  private syncTransport(): void {
    const t = this.transport_;
    if (!t) return;
    const host = this.state.hostId;
    const online = !!host && this.state.hosts.some((h) => h.deviceId === host && h.online);
    const usable = this.ready && !!this.ws && this.ws.readyState === WS_OPEN && online;
    if (t.readyState === WS_OPEN) {
      if (!usable || t.host !== host) this.dropTransport();
    } else if (t.readyState === 0 && usable) t.opened(host!);
  }

  private dropTransport(): void {
    const t = this.transport_;
    this.transport_ = null;
    t?.lost();
  }

  /** The renewal socket delivered `hosts`: it becomes the current socket, the old one is closed. */
  private promote(): void {
    const old = this.ws;
    this.ws = this.renewing;
    this.renewing = null;
    this.retire(old);
    this.lastRx = this.o.now();
    this.pingSentAt = null;
    this.startTimers();
    if (!this.visible) this.sendFrame({ t: 'pause' });
  }

  private socketLost(ws: WebSocketLike): void {
    if (ws === this.renewing) {
      this.renewing = null;
      if (this.ws) this.scheduleRenew(RENEW_RETRY_MS);
      return;
    }
    if (ws !== this.ws) return;
    this.retire(ws);
    // the server may close the old socket before the renewal delivered `hosts`: keep going on the new one
    if (this.renewing && this.renewing.readyState === WS_OPEN) {
      this.ws = this.renewing;
      this.renewing = null;
      this.lastRx = this.o.now();
      this.startTimers();
      return;
    }
    this.retire(this.renewing);
    this.renewing = null;
    this.ws = null;
    this.ready = false;
    this.clearTimers();
    this.pingSentAt = null;
    this.dropTransport();
    if (this.wantOpen) this.scheduleReconnect();
    else this.set({ status: 'idle' });
  }

  private scheduleReconnect(): void {
    this.attempt++;
    this.set({ status: 'reconnecting', failures: this.state.failures + 1 });
    const { backoffInitialMs, backoffMaxMs, jitter } = this.o;
    const base = Math.min(backoffMaxMs, backoffInitialMs * Math.pow(2, this.attempt - 1));
    const delay = base * (1 - jitter * Math.random());
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      // the 10-min session cookie may have expired while away: refresh first (errors → try anyway)
      void this.refresh()
        .catch(() => undefined)
        .then(() => {
          if (this.wantOpen && !this.ws && !this.reconnectTimer) this.open();
        });
    }, delay);
  }

  private ping(): void {
    this.lastPing = this.o.now();
    if (this.sendFrame({ t: 'ping' })) this.pingSentAt = this.lastPing;
  }

  private startTimers(): void {
    if (this.heartbeat) clearInterval(this.heartbeat);
    this.lastPing = this.o.now();
    this.heartbeat = setInterval(() => {
      const ws = this.ws;
      if (!ws || ws.readyState !== WS_OPEN) return;
      const now = this.o.now();
      if (now - this.lastRx > this.o.silenceMs) return this.socketLost(ws);
      if (now - this.lastPing >= this.o.pingMs) this.ping();
    }, Math.min(HEARTBEAT_TICK_MS, this.o.pingMs));
    this.scheduleRenew(this.o.renewMs);
  }

  private scheduleRenew(ms: number): void {
    if (this.renewTimer) clearTimeout(this.renewTimer);
    this.renewTimer = setTimeout(() => {
      this.renewTimer = null;
      void this.refresh()
        .catch(() => undefined)
        .then(() => {
          if (!this.ws || this.ws.readyState !== WS_OPEN || this.renewing) return;
          const ws = this.connectSocket();
          if (ws) this.renewing = ws;
          else this.scheduleRenew(RENEW_RETRY_MS);
        });
    }, ms);
  }

  private clearTimers(): void {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    if (this.heartbeat) clearInterval(this.heartbeat);
    if (this.renewTimer) clearTimeout(this.renewTimer);
    this.reconnectTimer = this.heartbeat = this.renewTimer = null;
  }
}

/** EngineClient's socket while the route is the relay: one host, wrap/unwrap, `set_param` coalescing. */
export class RelayTransport implements WebSocketLike {
  readyState = 0;
  readonly strictReplies = true;
  /** deviceId this transport talks to (set when it opens) */
  host: string | null = null;
  onopen: ((ev: unknown) => void) | null = null;
  onclose: ((ev: unknown) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  private params = new Map<string, unknown>();
  private paramTimer: ReturnType<typeof setTimeout> | null = null;
  private lastParamAt = -Infinity;

  constructor(
    private readonly link: RelayLink,
    private readonly paramIntervalMs: number,
    private readonly now: () => number,
  ) {}

  send(data: string): void {
    if (this.readyState !== WS_OPEN) return;
    let m: Record<string, unknown>;
    try {
      m = JSON.parse(data) as Record<string, unknown>;
    } catch {
      return;
    }
    if (m.type === 'set_param' && m.id === undefined) {
      const key = `${String(m.node)}/${String(m.param)}`;
      const t = this.now();
      if (!this.paramTimer && t - this.lastParamAt >= this.paramIntervalMs) {
        this.lastParamAt = t;
        this.link.sendMsg(this, m);
        return;
      }
      this.params.delete(key); // latest value, sent in the order of the latest edits
      this.params.set(key, m);
      if (!this.paramTimer)
        this.paramTimer = setTimeout(() => this.flushParams(), Math.max(0, this.paramIntervalMs - (t - this.lastParamAt)));
      return;
    }
    this.flushParams(); // earlier edits go out before anything that follows them
    this.link.sendMsg(this, m);
  }

  close(): void {
    if (this.readyState === 3) return;
    this.flushParams();
    this.readyState = 3;
    this.clearParams();
    this.link.detach(this);
  }

  /** @internal relay socket open, host online */
  opened(host: string): void {
    this.host = host;
    this.readyState = WS_OPEN;
    this.onopen?.({});
  }

  /** @internal engine message from the host */
  deliver(text: string): void {
    this.onmessage?.({ data: text });
  }

  /** @internal relay socket gone, host offline or another host selected */
  lost(): void {
    if (this.readyState === 3) return;
    this.readyState = 3;
    this.clearParams();
    this.onclose?.({});
  }

  private flushParams(): void {
    if (this.paramTimer) clearTimeout(this.paramTimer);
    this.paramTimer = null;
    if (!this.params.size || this.readyState !== WS_OPEN) return;
    this.lastParamAt = this.now();
    for (const m of this.params.values()) this.link.sendMsg(this, m);
    this.params.clear();
  }

  private clearParams(): void {
    if (this.paramTimer) clearTimeout(this.paramTimer);
    this.paramTimer = null;
    this.params.clear();
  }
}
