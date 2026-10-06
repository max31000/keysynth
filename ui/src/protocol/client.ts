// WebSocket client for the keysynth engine (docs/PROTOCOL.md).
// - auto-reconnect with exponential backoff (+ jitter)
// - request/response correlated by `id`, with timeout
// - typed event subscription

import type { ClientMsg, EngineMsg, EngineMsgOf, EngineMsgType, ErrorReply } from './types';

export const DEFAULT_ENGINE_URL = 'ws://127.0.0.1:7341';

export type ConnectionStatus = 'idle' | 'connecting' | 'open' | 'reconnecting' | 'closed';

/** Minimal WebSocket surface we use, so tests can inject a fake. */
export interface WebSocketLike {
  readonly readyState: number;
  send(data: string): void;
  close(code?: number, reason?: string): void;
  onopen: ((ev: unknown) => void) | null;
  onclose: ((ev: unknown) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
}
export type WebSocketFactory = (url: string) => WebSocketLike;

export interface ClientOptions {
  url?: string;
  requestTimeoutMs?: number;
  backoffInitialMs?: number;
  backoffMaxMs?: number;
  backoffFactor?: number;
  /** 0..1, fraction of the delay randomised; 0 in tests */
  jitter?: number;
  createSocket?: WebSocketFactory;
}

export class EngineError extends Error {
  constructor(
    public readonly code: string,
    message: string,
  ) {
    super(message);
    this.name = 'EngineError';
  }
}

/** Without-`id` variant of any client message (the client assigns ids). */
type Outgoing = ClientMsg extends infer M ? (M extends { id?: number } ? Omit<M, 'id'> : never) : never;

interface Pending {
  resolve: (m: EngineMsg) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
  expect?: EngineMsgType;
}

type Handler = (msg: EngineMsg) => void;

const WS_OPEN = 1;

/** Resolve engine URL from `?engine=` (host:port or full ws:// url). */
export function engineUrlFromLocation(search: string, fallback = DEFAULT_ENGINE_URL): string {
  const q = new URLSearchParams(search).get('engine');
  if (!q) return fallback;
  if (/^wss?:\/\//.test(q)) return q;
  return `ws://${q}`;
}

export class EngineClient {
  readonly url: string;
  private readonly opts: Required<Omit<ClientOptions, 'url' | 'createSocket'>>;
  private readonly createSocket: WebSocketFactory;
  private ws: WebSocketLike | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private handlers = new Map<string, Set<Handler>>();
  private statusHandlers = new Set<(s: ConnectionStatus, attempt: number) => void>();
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private attempt = 0;
  private wantOpen = false;
  private _status: ConnectionStatus = 'idle';

  constructor(options: ClientOptions = {}) {
    this.url = options.url ?? DEFAULT_ENGINE_URL;
    this.opts = {
      requestTimeoutMs: options.requestTimeoutMs ?? 5000,
      backoffInitialMs: options.backoffInitialMs ?? 250,
      backoffMaxMs: options.backoffMaxMs ?? 5000,
      backoffFactor: options.backoffFactor ?? 2,
      jitter: options.jitter ?? 0.2,
    };
    this.createSocket =
      options.createSocket ?? ((url) => new WebSocket(url) as unknown as WebSocketLike);
  }

  get status(): ConnectionStatus {
    return this._status;
  }
  get isOpen(): boolean {
    return this.ws?.readyState === WS_OPEN && this._status === 'open';
  }

  connect(): void {
    this.wantOpen = true;
    if (this.ws) return;
    this.open();
  }

  close(): void {
    this.wantOpen = false;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    const ws = this.ws;
    this.ws = null;
    if (ws) {
      ws.onclose = ws.onerror = ws.onmessage = ws.onopen = null;
      ws.close();
    }
    this.failAll(new EngineError('closed', 'connection closed'));
    this.setStatus('closed');
  }

  /** Delay before reconnect attempt n (1-based), without jitter. */
  backoffDelay(n: number): number {
    const { backoffInitialMs, backoffFactor, backoffMaxMs } = this.opts;
    return Math.min(backoffMaxMs, backoffInitialMs * Math.pow(backoffFactor, Math.max(0, n - 1)));
  }

  /** Fire-and-forget. Returns false (message dropped) when not connected. */
  send(msg: Outgoing): boolean {
    if (!this.ws || this.ws.readyState !== WS_OPEN) return false;
    this.ws.send(JSON.stringify(msg));
    return true;
  }

  /**
   * Send with an `id` and wait for the reply carrying the same id (`<type>_ok`, `state`, `devices`, …).
   * `expect` is a fallback for engines that don't echo ids on event-typed replies: the first message of that
   * type with no id resolves the oldest pending request expecting it.
   */
  request<T extends EngineMsgType = EngineMsgType>(
    msg: Outgoing,
    options: { timeoutMs?: number; expect?: T } = {},
  ): Promise<EngineMsgOf<T>> {
    if (!this.ws || this.ws.readyState !== WS_OPEN) {
      return Promise.reject(new EngineError('disconnected', 'not connected to engine'));
    }
    const id = this.nextId++;
    const timeoutMs = options.timeoutMs ?? this.opts.requestTimeoutMs;
    return new Promise<EngineMsgOf<T>>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new EngineError('timeout', `${msg.type} timed out after ${timeoutMs} ms`));
      }, timeoutMs);
      const p: Pending = { resolve: resolve as (m: EngineMsg) => void, reject, timer };
      if (options.expect) p.expect = options.expect;
      this.pending.set(id, p);
      this.ws!.send(JSON.stringify({ ...msg, id }));
    });
  }

  /** Subscribe to an engine message type (or '*' for all). Returns an unsubscribe function. */
  on<T extends EngineMsgType>(type: T | '*', handler: (msg: EngineMsgOf<T>) => void): () => void {
    let set = this.handlers.get(type);
    if (!set) this.handlers.set(type, (set = new Set()));
    set.add(handler as Handler);
    return () => set!.delete(handler as Handler);
  }

  onStatus(handler: (s: ConnectionStatus, attempt: number) => void): () => void {
    this.statusHandlers.add(handler);
    return () => this.statusHandlers.delete(handler);
  }

  // ─────────────── internals ───────────────

  private open(): void {
    this.setStatus(this.attempt === 0 ? 'connecting' : 'reconnecting');
    let ws: WebSocketLike;
    try {
      ws = this.createSocket(this.url);
    } catch {
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    ws.onopen = () => {
      this.attempt = 0;
      this.setStatus('open');
    };
    ws.onmessage = (ev) => this.handleRaw(ev.data);
    ws.onerror = () => {
      /* onclose follows */
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.failAll(new EngineError('disconnected', 'connection lost'));
      if (this.wantOpen) this.scheduleReconnect();
      else this.setStatus('closed');
    };
  }

  private scheduleReconnect(): void {
    this.attempt++;
    this.setStatus('reconnecting');
    const base = this.backoffDelay(this.attempt);
    const delay = base * (1 - this.opts.jitter * Math.random());
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (this.wantOpen && !this.ws) this.open();
    }, delay);
  }

  private handleRaw(data: unknown): void {
    if (typeof data !== 'string') return;
    let msg: EngineMsg;
    try {
      msg = JSON.parse(data) as EngineMsg;
    } catch {
      return;
    }
    if (!msg || typeof msg !== 'object' || typeof msg.type !== 'string') return;
    this.resolvePending(msg);
    this.dispatch(msg);
  }

  private resolvePending(msg: EngineMsg): void {
    const id = (msg as { id?: number }).id;
    let key: number | undefined;
    if (typeof id === 'number' && this.pending.has(id)) key = id;
    else if (id === undefined) {
      for (const [k, p] of this.pending) {
        if (p.expect === msg.type) {
          key = k;
          break;
        }
      }
    }
    if (key === undefined) return;
    const p = this.pending.get(key)!;
    this.pending.delete(key);
    clearTimeout(p.timer);
    if (msg.type === 'error') {
      const e = msg as ErrorReply;
      p.reject(new EngineError(e.code, e.message));
    } else p.resolve(msg);
  }

  private dispatch(msg: EngineMsg): void {
    for (const key of [msg.type, '*']) {
      const set = this.handlers.get(key);
      if (!set) continue;
      for (const h of [...set]) {
        try {
          h(msg);
        } catch (err) {
          console.error('[engine client] handler error', err);
        }
      }
    }
  }

  private failAll(err: Error): void {
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pending.clear();
  }

  private setStatus(s: ConnectionStatus): void {
    if (s === this._status && s !== 'reconnecting') return;
    this._status = s;
    for (const h of [...this.statusHandlers]) h(s, this.attempt);
  }
}
