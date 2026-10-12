// Is the server on the engine port really a keysynth engine? (PROTOCOL.md "Hello additions" → `engine`)
// A leftover dev mock or another program on 127.0.0.1:7341 must not be driven as if it were the engine.

import type { StateEvent } from '../protocol/types';

export type IdentityCheck =
  /** no `hello` reply yet (or not connected) */
  | 'pending'
  | 'engine'
  /** engine before 0.1.1 (no `engine` field, but the other hello additions) */
  | 'legacy'
  | 'mock'
  /** wrong / missing identity, an error or no reply to `hello` */
  | 'foreign';

/** `cloud.deviceId` of the mock before it reported `kind: "mock"` (also the PROTOCOL.md example). */
export const OLD_MOCK_DEVICE_ID = '3f0c7e9a-5b1d-4c2e-9a77-0e5d2c1b8f40';

export function checkIdentity(reply: unknown): IdentityCheck {
  if (!reply || typeof reply !== 'object') return 'foreign';
  const m = reply as Partial<StateEvent> & { engine?: unknown };
  if (m.type !== 'state') return 'foreign';
  const e = m.engine as { product?: unknown; kind?: unknown } | undefined;
  if (e !== undefined) {
    if (!e || typeof e !== 'object' || e.product !== 'keysynth') return 'foreign';
    return e.kind === 'engine' ? 'engine' : e.kind === 'mock' ? 'mock' : 'foreign';
  }
  if (typeof m.version !== 'string' || !m.trust || !m.cloud) return 'foreign';
  return m.cloud.deviceId === OLD_MOCK_DEVICE_ID ? 'mock' : 'legacy';
}

/** Dev builds may drive the mock (with a MOCK badge); production pages only a real engine. */
export function identityAccepted(c: IdentityCheck, dev: boolean): boolean {
  return c === 'engine' || c === 'legacy' || (c === 'mock' && dev);
}

/** The UI must show «не движок keysynth» instead of the synth. */
export function identityRejected(c: IdentityCheck, dev: boolean): boolean {
  return c !== 'pending' && !identityAccepted(c, dev);
}
