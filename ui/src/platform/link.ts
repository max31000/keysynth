// Automatic engine ⇄ account link (spec §3, §11; PROTOCOL.md `cloud_link`; REST: POST /api/devices).
//
// The page (platform cookies) mints a device token and hands it straight to the local engine over the
// WebSocket. The token lives only in local variables of `linkEngine()`: never in a store, storage, URL or log.

import type { CloudStatus } from '../protocol/types';

export const REMINT_BEFORE_MS = 30 * 24 * 3600 * 1000;

/** Who owns the engine's current link, as far as the page can tell (GET /api/devices, cookie). */
export type Owner = 'unknown' | 'mine' | 'foreign';

export type LinkPlan =
  | { kind: 'idle'; reason: 'local-mode' | 'no-engine' | 'auth-loading' | 'auth-unavailable' | 'remote' }
  | { kind: 'login' }
  | { kind: 'no-access' }
  /** engine not linked → mint + cloud_link */
  | { kind: 'link' }
  /** linked: find out whether it is our account */
  | { kind: 'check-owner' }
  /** linked to someone else: wait for the user's explicit confirmation (never automatic) */
  | { kind: 'foreign' }
  /** our link expires in < 30 days → re-mint silently */
  | { kind: 'remint' }
  | { kind: 'ok' };

export interface PlanInput {
  mode: 'platform' | 'local';
  connected: boolean;
  cloud: CloudStatus | null;
  meStatus: 'loading' | 'unavailable' | 'anonymous' | 'authenticated';
  granted: boolean;
  owner: Owner;
  now: number;
  /** connected through the relay (trust `remote`): the host is linked already and may not be re-linked */
  remote?: boolean;
}

export function planLink(i: PlanInput): LinkPlan {
  if (i.mode !== 'platform') return { kind: 'idle', reason: 'local-mode' };
  if (!i.connected || !i.cloud) return { kind: 'idle', reason: 'no-engine' };
  if (i.remote) return { kind: 'idle', reason: 'remote' };
  if (i.meStatus === 'loading') return { kind: 'idle', reason: 'auth-loading' };
  if (i.meStatus === 'unavailable') return { kind: 'idle', reason: 'auth-unavailable' };
  if (i.meStatus === 'anonymous') return { kind: 'login' };
  if (!i.granted) return { kind: 'no-access' };
  if (!i.cloud.linked) return { kind: 'link' };
  if (i.owner === 'unknown') return { kind: 'check-owner' };
  if (i.owner === 'foreign') return { kind: 'foreign' };
  const exp = i.cloud.expiresAt ? Date.parse(i.cloud.expiresAt) : NaN;
  if (Number.isFinite(exp) && exp - i.now < REMINT_BEFORE_MS) return { kind: 'remint' };
  return { kind: 'ok' };
}

export interface DeviceRow {
  deviceId: string;
  name: string;
  expiresAt: string;
}

/**
 * The engine's link is ours iff our device list has its deviceId with the same expiry: every mint (per user +
 * device) sets a fresh `expiresAt`, so another account's link to this PC carries a different one.
 */
export function ownerFromDevices(cloud: CloudStatus, items: DeviceRow[]): Owner {
  const row = items.find((d) => d.deviceId === cloud.deviceId);
  if (!row || !cloud.expiresAt) return 'foreign';
  const a = Date.parse(row.expiresAt);
  const b = Date.parse(cloud.expiresAt);
  return Number.isFinite(a) && Number.isFinite(b) && Math.abs(a - b) < 2000 ? 'mine' : 'foreign';
}

export interface Minted {
  token: string;
  expiresAt: string;
  accountId: string;
  accountName: string;
}

export interface LinkDeps {
  /** POST /keysynth/api/devices { deviceId, name } (cookie) */
  mint(deviceId: string, name: string): Promise<Minted>;
  /** `cloud_link` over the engine socket; rejects with an error carrying `code` (`account_mismatch`, …) */
  sendLink(token: string, replace: boolean): Promise<CloudStatus & { merged?: { presets: number; patterns: number } }>;
}

export type LinkResult =
  | { ok: true; status: CloudStatus; merged?: { presets: number; patterns: number } | undefined; expiresAt: string; accountId: string }
  | { ok: false; code: string; message: string };

function errCode(e: unknown): { code: string; message: string } {
  const o = e as { code?: unknown; status?: unknown; message?: unknown } | null;
  const code = typeof o?.code === 'string' && o.code ? o.code : typeof o?.status === 'number' ? `http_${o.status}` : 'error';
  // Messages come from our API / engine (`{error}` codes, engine text) and never contain the token.
  const message = typeof o?.message === 'string' ? o.message : String(e);
  return { code, message };
}

/**
 * Mint + link. `replace` = the user confirmed switching the engine to this account. Without it an engine linked
 * to another account answers `account_mismatch` (nothing changes) and the caller asks the user.
 */
export async function linkEngine(
  deps: LinkDeps,
  deviceId: string,
  deviceName: string,
  replace: boolean,
  /** before handing the token over: false → `account_mismatch` (the token is dropped, nothing is sent) */
  accept: (m: Pick<Minted, 'accountId' | 'accountName'>) => boolean = () => true,
): Promise<LinkResult> {
  let minted: Minted;
  try {
    minted = await deps.mint(deviceId, deviceName);
  } catch (e) {
    return { ok: false, ...errCode(e) };
  }
  if (!replace && !accept(minted)) return { ok: false, code: 'account_mismatch', message: 'another account was linked before' };
  try {
    const status = await deps.sendLink(minted.token, replace);
    return { ok: true, status, merged: status.merged, expiresAt: minted.expiresAt, accountId: minted.accountId };
  } catch (e) {
    return { ok: false, ...errCode(e) };
  }
}

/** Human-readable reason for a failed link (Russian UI copy). */
export function linkErrorText(code: string): string {
  switch (code) {
    case 'offline':
      return 'Движок не может связаться с mvv42.ru — проверьте интернет.';
    case 'token_invalid':
      return 'Сервер отклонил ключ устройства. Попробуйте ещё раз.';
    case 'too_many_devices':
      return 'К аккаунту привязано слишком много устройств (максимум 10).';
    case 'rate_limited':
      return 'Слишком много попыток привязки — подождите немного.';
    case 'unauthorized':
    case 'http_401':
      return 'Сессия истекла — войдите снова.';
    case 'forbidden':
    case 'http_403':
      return 'Нет доступа к keysynth для этого аккаунта.';
    case 'disconnected':
    case 'timeout':
      return 'Движок не ответил. Проверьте, что keysynth запущен.';
    default:
      return `Не удалось привязать движок (${code}).`;
  }
}

/** Device name for POST /api/devices (1..100 chars). The engine knows %COMPUTERNAME%, the page does not. */
export function defaultDeviceName(platform: string | undefined): string {
  const p = (platform ?? '').toLowerCase();
  const os = p.includes('win') ? 'Windows' : p.includes('mac') ? 'macOS' : p.includes('linux') ? 'Linux' : 'PC';
  return `keysynth · ${os}`;
}
