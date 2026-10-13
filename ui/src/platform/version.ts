// Engine version vs the latest GitHub release (spec §11: the UI queries GitHub directly).

import type { DesktopOs } from './os';

export const RELEASES_API: string =
  (import.meta.env.VITE_KS_RELEASES_API as string | undefined) ??
  'https://api.github.com/repos/max31000/keysynth/releases/latest';
const LATEST_DOWNLOAD = 'https://github.com/max31000/keysynth/releases/latest/download/';
/** Fixed-name release assets (scripts/build_installer.ps1, scripts/build_app_macos.sh). */
export const DOWNLOAD_URLS = {
  windows: LATEST_DOWNLOAD + 'keysynth-setup.exe',
  mac: LATEST_DOWNLOAD + 'keysynth-macos.dmg',
} as const;
/** Download for this OS; other OSes (no engine build) get the Windows installer link as before. */
export const downloadUrl = (os: DesktopOs): string => (os === 'mac' ? DOWNLOAD_URLS.mac : DOWNLOAD_URLS.windows);
export const RELEASES_PAGE = 'https://github.com/max31000/keysynth/releases/latest';

export type Semver = [number, number, number];

/** `"v1.2.3"`, `"1.2.3"`, `"1.2.3-rc.1"` (pre-release / build suffix ignored) → [1, 2, 3]; null if unparsable. */
export function parseSemver(v: string | null | undefined): Semver | null {
  if (typeof v !== 'string') return null;
  const m = /^\s*v?(\d+)\.(\d+)(?:\.(\d+))?(?:[-+].*)?\s*$/.exec(v);
  if (!m) return null;
  return [Number(m[1]), Number(m[2]), Number(m[3] ?? 0)];
}

/** <0 when a < b, 0 equal, >0 when a > b. */
export function compareSemver(a: Semver, b: Semver): number {
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i]! - b[i]!;
  return 0;
}

export interface LatestRelease {
  version: string;
  /** release page */
  url: string;
}

/**
 * Engine is outdated when the latest release is newer. An engine that reports no version predates the cloud
 * protocol: outdated whenever any release is known. Unknown latest → never outdated.
 */
export function isOutdated(engineVersion: string | null | undefined, latest: LatestRelease | null): boolean {
  const l = parseSemver(latest?.version);
  if (!l) return false;
  const e = parseSemver(engineVersion);
  if (!e) return true;
  return compareSemver(e, l) < 0;
}

/** GET the GitHub latest release. Network errors / 404 (no release yet) / rate limits → null. */
export async function fetchLatestRelease(fetchFn: typeof fetch = fetch, apiUrl = RELEASES_API): Promise<LatestRelease | null> {
  try {
    const r = await fetchFn(apiUrl, { headers: { Accept: 'application/vnd.github+json' }, credentials: 'omit' });
    if (!r.ok) return null;
    const j = (await r.json()) as { tag_name?: unknown; html_url?: unknown; draft?: unknown; prerelease?: unknown };
    if (typeof j.tag_name !== 'string' || j.draft === true || j.prerelease === true) return null;
    const v = parseSemver(j.tag_name);
    if (!v) return null;
    const page = typeof j.html_url === 'string' && j.html_url.startsWith('https://github.com/') ? j.html_url : RELEASES_PAGE;
    return { version: v.join('.'), url: page };
  } catch {
    return null;
  }
}
