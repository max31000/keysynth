import { describe, expect, it, vi } from 'vitest';
import { compareSemver, fetchLatestRelease, isOutdated, parseSemver } from '../src/platform/version';

describe('semver', () => {
  it('parses tags with and without v, pre-release suffixes, two-part versions', () => {
    expect(parseSemver('v1.2.3')).toEqual([1, 2, 3]);
    expect(parseSemver('0.4.0')).toEqual([0, 4, 0]);
    expect(parseSemver('1.10.0-rc.1')).toEqual([1, 10, 0]);
    expect(parseSemver('2.1')).toEqual([2, 1, 0]);
    expect(parseSemver('latest')).toBeNull();
    expect(parseSemver(undefined)).toBeNull();
  });

  it('compares numerically (1.10 > 1.9)', () => {
    expect(compareSemver([1, 10, 0], [1, 9, 9])).toBeGreaterThan(0);
    expect(compareSemver([0, 4, 0], [0, 4, 0])).toBe(0);
    expect(compareSemver([0, 3, 9], [0, 4, 0])).toBeLessThan(0);
  });

  it('outdated only when a newer release is known; versionless engines are outdated', () => {
    const rel = (version: string) => ({ version, url: 'https://github.com/x' });
    expect(isOutdated('0.1.0', rel('0.2.0'))).toBe(true);
    expect(isOutdated('0.2.0', rel('0.2.0'))).toBe(false);
    expect(isOutdated('0.3.0', rel('0.2.0'))).toBe(false); // dev build ahead of the release
    expect(isOutdated(null, rel('0.2.0'))).toBe(true);
    expect(isOutdated('0.1.0', null)).toBe(false);
  });
});

describe('fetchLatestRelease', () => {
  const resp = (status: number, body: unknown) =>
    Promise.resolve(new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } }));

  it('reads tag_name and keeps only github.com release pages', async () => {
    const f = vi.fn(() => resp(200, { tag_name: 'v0.5.1', html_url: 'https://github.com/max31000/keysynth/releases/tag/v0.5.1' }));
    expect(await fetchLatestRelease(f as unknown as typeof fetch, 'x')).toEqual({
      version: '0.5.1',
      url: 'https://github.com/max31000/keysynth/releases/tag/v0.5.1',
    });
    const evil = vi.fn(() => resp(200, { tag_name: 'v0.5.1', html_url: 'javascript:alert(1)' }));
    expect((await fetchLatestRelease(evil as unknown as typeof fetch, 'x'))?.url).toBe('https://github.com/max31000/keysynth/releases/latest');
  });

  it('404 (no release yet), drafts, garbage and network errors → null', async () => {
    expect(await fetchLatestRelease((() => resp(404, { message: 'Not Found' })) as unknown as typeof fetch, 'x')).toBeNull();
    expect(await fetchLatestRelease((() => resp(200, { tag_name: 'v1.0.0', draft: true })) as unknown as typeof fetch, 'x')).toBeNull();
    expect(await fetchLatestRelease((() => resp(200, { tag_name: 'nightly' })) as unknown as typeof fetch, 'x')).toBeNull();
    expect(await fetchLatestRelease((() => Promise.reject(new TypeError('offline'))) as unknown as typeof fetch, 'x')).toBeNull();
  });
});
