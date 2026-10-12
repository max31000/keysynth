// Engine discovery for the platform page (spec §8, §11 Browser).
//
// A public page reaching ws://127.0.0.1 triggers Chrome's Local Network Access prompt, so the page probes the
// engine on its own only when it has seen the engine before (localStorage) or the permission is already granted;
// otherwise it waits for «Подключить движок». After the first contact the EngineClient keeps reconnecting with
// backoff; this module turns its socket status + the permission state into a phase the UI renders.

import type { ConnectionStatus, EngineClient } from '../protocol/client';

export type EnginePhase =
  /** never connected from this browser and not asked yet → download card + «Подключить движок» */
  | 'never-seen'
  /** connection attempt in flight (or the LNA prompt is open) */
  | 'connecting'
  /** the browser blocks local network access for this site */
  | 'permission-denied'
  /** attempts fail (engine not installed / not started); retrying in the background */
  | 'not-running'
  | 'connected';

export type PermissionState = 'granted' | 'denied' | 'prompt' | 'unknown';

export interface DiscoveryState {
  phase: EnginePhase;
  /** the engine answered from this browser before (persisted) */
  seen: boolean;
  /** discovery is active (auto-probe or the user asked) */
  started: boolean;
  permission: PermissionState;
  /** failed attempts since the last open socket */
  failures: number;
}

export type DiscoveryEvent =
  | { type: 'start' }
  | { type: 'socket'; status: ConnectionStatus }
  | { type: 'permission'; state: PermissionState };

/** Failed attempts before «not running» is shown (the first retry follows within ~250 ms). */
export const FAILURES_BEFORE_NOT_RUNNING = 2;

export function initialDiscovery(seen: boolean, permission: PermissionState = 'unknown'): DiscoveryState {
  const auto = seen || permission === 'granted';
  const base: DiscoveryState = { phase: 'never-seen', seen, started: auto, permission, failures: 0 };
  return { ...base, phase: phaseOf(base) };
}

/** Should the page open the socket without a click? */
export function shouldAutoProbe(s: DiscoveryState): boolean {
  return s.seen || s.permission === 'granted';
}

/**
 * `permissions.query` is only a hint: it can say `denied` where the socket works anyway (pages served from a
 * loopback/private origin are not subject to LNA; embedded browsers report `denied` for unknown features). So a
 * denied permission is shown only after an attempt actually failed, and an open socket always wins.
 */
function phaseOf(s: DiscoveryState): EnginePhase {
  if (!s.started) return 'never-seen';
  if (s.permission === 'denied' && s.failures >= 1) return 'permission-denied';
  return s.failures >= FAILURES_BEFORE_NOT_RUNNING ? 'not-running' : 'connecting';
}

export function reduceDiscovery(s: DiscoveryState, e: DiscoveryEvent): DiscoveryState {
  switch (e.type) {
    case 'start': {
      const n = { ...s, started: true, failures: 0 };
      return { ...n, phase: s.phase === 'connected' ? 'connected' : phaseOf(n) };
    }
    case 'permission': {
      const n = { ...s, permission: e.state, started: s.started || e.state === 'granted' };
      return { ...n, phase: s.phase === 'connected' ? 'connected' : phaseOf(n) };
    }
    case 'socket': {
      if (e.status === 'open') return { ...s, seen: true, started: true, failures: 0, phase: 'connected' };
      if (e.status === 'reconnecting') {
        const n = { ...s, failures: s.failures + 1 };
        return { ...n, phase: phaseOf(n) };
      }
      if (e.status === 'connecting') return { ...s, phase: s.phase === 'connected' ? 'connecting' : phaseOf(s) };
      // idle / closed (client stopped)
      return { ...s, phase: s.phase === 'connected' ? 'not-running' : phaseOf(s) };
    }
  }
}

// ───────────────────────────── browser glue ─────────────────────────────

export const ENGINE_SEEN_KEY = 'keysynth.engineSeen';

export interface StorageLike {
  getItem(k: string): string | null;
  setItem(k: string, v: string): void;
}

export function readSeen(storage: StorageLike | null): boolean {
  try {
    return storage?.getItem(ENGINE_SEEN_KEY) === '1';
  } catch {
    return false;
  }
}

export function writeSeen(storage: StorageLike | null): void {
  try {
    storage?.setItem(ENGINE_SEEN_KEY, '1');
  } catch {
    /* private mode */
  }
}

/** Permission names Chrome has used for loopback access from public pages (LNA); unknown names throw. */
const LNA_PERMISSIONS = ['loopback-network', 'local-network-access', 'local-network'];

export interface PermissionsLike {
  query(d: { name: string }): Promise<{ state: string; onchange: ((this: unknown, ev: unknown) => unknown) | null }>;
}

/**
 * Current LNA permission (first name the browser knows) and a change subscription. Browsers without LNA →
 * 'unknown' (they allow loopback WebSockets without asking).
 */
export async function watchLnaPermission(
  permissions: PermissionsLike | undefined,
  onChange: (s: PermissionState) => void,
): Promise<PermissionState> {
  if (!permissions?.query) return 'unknown';
  for (const name of LNA_PERMISSIONS) {
    try {
      const st = await permissions.query({ name });
      const norm = (v: string): PermissionState => (v === 'granted' || v === 'denied' || v === 'prompt' ? v : 'unknown');
      st.onchange = () => onChange(norm(st.state));
      return norm(st.state);
    } catch {
      /* not a permission name in this browser */
    }
  }
  return 'unknown';
}

export interface DiscoveryDeps {
  client: Pick<EngineClient, 'connect' | 'retryNow' | 'onStatus'>;
  storage: StorageLike | null;
  permissions?: PermissionsLike | undefined;
  onState: (s: DiscoveryState) => void;
}

/** Wires reducer ⇄ EngineClient ⇄ localStorage ⇄ permission. Returns `start()` for the «Подключить» button. */
export function createDiscovery(deps: DiscoveryDeps) {
  let state = initialDiscovery(readSeen(deps.storage));
  const emit = (e: DiscoveryEvent) => {
    const prev = state;
    state = reduceDiscovery(state, e);
    if (state.seen && !prev.seen) writeSeen(deps.storage);
    if (state.started && !prev.started) deps.client.connect();
    // granted while waiting in backoff (up to 10 s): try right away
    else if (e.type === 'permission' && e.state === 'granted' && prev.permission !== 'granted' && state.phase !== 'connected')
      deps.client.retryNow();
    if (state !== prev) deps.onState(state);
  };
  deps.onState(state);
  if (state.started) deps.client.connect();
  const off = deps.client.onStatus((status) => emit({ type: 'socket', status }));
  void watchLnaPermission(deps.permissions, (p) => emit({ type: 'permission', state: p })).then((p) => {
    if (p !== 'unknown') emit({ type: 'permission', state: p });
  });
  return {
    get state() {
      return state;
    },
    /** user clicked «Подключить движок» / «Повторить» */
    start() {
      const wasStarted = state.started;
      emit({ type: 'start' });
      // already running: kick an immediate attempt instead of waiting for the backoff timer
      if (wasStarted) deps.client.retryNow();
    },
    dispose: off,
  };
}
