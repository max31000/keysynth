// Which way the page reaches an engine (docs/specs/2026-10-12-remote.md §6, §10.12):
// - `direct`: ws://127.0.0.1:7341 (local UI; platform page on the PC that runs the engine; signed-out visitors)
// - `relay`: through mvv42.ru (platform page, signed in with access, and this browser has no local engine)

import type { AppMode } from './mode';

export type Route = 'direct' | 'relay';

export interface RouteInput {
  mode: AppMode;
  me: { status: 'loading' | 'unavailable' | 'anonymous' | 'authenticated'; granted: boolean };
  /** discovery: the engine answered from this browser before / the user asked to connect the local engine */
  discovery: { seen: boolean; started: boolean };
  /** dev aid (`?remote=1|0`) */
  force?: Route | null;
}

export function decideRoute(i: RouteInput): Route {
  if (i.mode !== 'platform') return 'direct';
  if (i.force) return i.force;
  // a PC tab with a local engine never opens the relay
  if (i.discovery.seen || i.discovery.started) return 'direct';
  return i.me.status === 'authenticated' && i.me.granted ? 'relay' : 'direct';
}
