// Which engine messages the current session may send (PROTOCOL.md "Sessions, Origin, Host"; remote trust:
// docs/specs/2026-10-12-remote.md §10.3). The engine is the authority; the UI only hides what would be refused.

import type { SessionTrust } from '../protocol/types';

/** Denied for `web` sessions (everything else the UI sends is allowed for them). */
const WEB_DENIED = new Set(['reload_plugin']);
/** Remote (relay) = web restrictions + these. */
const REMOTE_DENIED = new Set([
  ...WEB_DENIED,
  'cloud_link',
  'cloud_unlink',
  'set_audio_device',
  'open_audio_panel',
  'samples_delete',
]);

/** May a session with this trust send `type`? `null` (not connected yet) = optimistic yes. */
export function canSend(trust: SessionTrust | null, type: string): boolean {
  if (trust === 'web') return !WEB_DENIED.has(type);
  if (trust === 'remote') return !REMOTE_DENIED.has(type);
  return true;
}
