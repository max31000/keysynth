// Where is this page running? (docs/specs/2026-10-12-cloud.md §8)
// - platform: served from https://mvv42.ru/keysynth/ (or a `--mode platform*` dev build): platform auth (cookies),
//   API at /keysynth/api, engine discovered at ws://127.0.0.1:7341 only after the user asks (Local Network Access).
// - local: served by the engine itself at http://127.0.0.1:7340 (offline fallback) or the plain dev server:
//   no platform auth, engine connected right away; cloud features point to mvv42.ru.

import { DEFAULT_ENGINE_URL, engineUrlFromLocation } from '../protocol/client';

export type AppMode = 'platform' | 'local';

export const SERVICE_ID = 'keysynth';
export const PLATFORM_ORIGIN = 'https://mvv42.ru';
export const PLATFORM_URL = `${PLATFORM_ORIGIN}/keysynth/`;
export const API_BASE = '/keysynth/api';
export const ENGINE_PORT_HTTP = '7340';

export interface ModeInfo {
  mode: AppMode;
  engineUrl: string;
  /** dev builds only: `?engine=` override is honoured */
  dev: boolean;
}

export interface LocationLike {
  hostname: string;
  port: string;
  search: string;
}

/**
 * `platformBuild` = the bundle was built with base `/keysynth/` (`npm run build:platform` / `dev:platform`).
 * The engine's own HTTP port always means local mode (the bundled offline UI), whatever the build.
 */
export function detectMode(loc: LocationLike, platformBuild: boolean, dev: boolean): ModeInfo {
  const isLoopback = loc.hostname === '127.0.0.1' || loc.hostname === 'localhost';
  const servedByEngine = isLoopback && loc.port === ENGINE_PORT_HTTP;
  const onPlatform = loc.hostname === 'mvv42.ru';
  const mode: AppMode = servedByEngine ? 'local' : onPlatform || platformBuild ? 'platform' : 'local';
  // Production platform pages always talk to 127.0.0.1 (never `localhost`, spec §11); `?engine=` is a dev aid.
  const engineUrl = dev || mode === 'local' ? engineUrlFromLocation(loc.search) : DEFAULT_ENGINE_URL;
  return { mode, engineUrl, dev };
}

const env = import.meta.env;
export const MODE: ModeInfo =
  typeof location !== 'undefined'
    ? detectMode(location, env.BASE_URL === '/keysynth/', !!env.DEV)
    : { mode: 'local', engineUrl: DEFAULT_ENGINE_URL, dev: true };
