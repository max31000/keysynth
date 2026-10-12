import { describe, expect, it } from 'vitest';
import {
  applyDone,
  entryIdFromSfz,
  formatBytes,
  formatEta,
  jobForEntry,
  jobForLibrary,
  jobFraction,
  jobsFromProgress,
  libraryStatus,
  patchSampleEntries,
  totals,
  viewFromList,
} from '../src/lib/samples';
import type { Patch, SampleJob, SamplesListOk } from '../src/protocol/types';

const MB = 1024 * 1024;
const list: SamplesListOk = {
  type: 'samples_list_ok',
  manifest: { state: 'ok', etag: '"x"', fetchedAt: '2026-10-12T14:00:00Z' },
  cacheBytes: 5 * MB,
  freeBytes: 100 * MB,
  groups: [
    {
      id: 'epiano',
      name: 'Electric piano',
      libraries: [
        {
          id: 'jr',
          name: 'jRhodes',
          licence: 'CC0',
          author: 'j',
          attribution: '',
          bytes: 30 * MB,
          cachedBytes: 5 * MB,
          status: 'partial',
          job: null,
          entries: [
            { id: 'jr/a.sfz', name: 'a', sfz: 'assets/samples/jr/a.sfz', bytes: 10 * MB, cachedBytes: 5 * MB, status: 'partial', job: null },
            { id: 'jr/b.sfz', name: 'b', sfz: 'assets/samples/jr/b.sfz', bytes: 20 * MB, cachedBytes: 0, status: 'missing', job: null },
          ],
        },
      ],
    },
  ],
};

const job = (id: string, bytesDone: number, bytesTotal: number, extra: Partial<SampleJob> = {}): SampleJob => ({
  id,
  scope: id === 'all' ? 'all' : (id.split(':')[0] as SampleJob['scope']),
  target: id === 'all' ? '' : id.slice(id.indexOf(':') + 1),
  state: 'running',
  bytesDone,
  bytesTotal,
  filesDone: 0,
  filesTotal: 1,
  rate: 0,
  ...extra,
});

describe('sample ids', () => {
  it('maps sampler state.sfz to entry ids', () => {
    expect(entryIdFromSfz('assets/samples/salamander/x/y.sfz')).toBe('salamander/x/y.sfz');
    expect(entryIdFromSfz('userdata/x.sfz')).toBeNull();
    expect(entryIdFromSfz('assets/samples/')).toBeNull();
    expect(entryIdFromSfz(42)).toBeNull();
  });

  it('collects the patch samplers once each', () => {
    const inst = (sfz?: string) => ({ type: 'sampler', params: {}, state: sfz ? { sfz } : {} });
    const patch = {
      layers: [{ instrument: inst('assets/samples/jr/a.sfz') }, { instrument: inst('assets/samples/jr/a.sfz') }, { instrument: inst() }],
    } as unknown as Patch;
    expect(patchSampleEntries(patch)).toEqual(['jr/a.sfz']);
    expect(patchSampleEntries(null)).toEqual([]);
  });
});

describe('jobs', () => {
  it('progress replaces the job map; most specific job wins', () => {
    const jobs = jobsFromProgress([job('all', 1, 10), job('library:jr', 2, 10), job('entry:jr/a.sfz', 3, 10)]);
    expect(jobForEntry(jobs, 'jr/a.sfz')?.id).toBe('entry:jr/a.sfz');
    expect(jobForEntry(jobs, 'jr/b.sfz')?.id).toBe('library:jr');
    expect(jobForEntry(jobs, 'other/c.sfz')?.id).toBe('all');
    expect(jobForLibrary(jobs, 'jr')?.id).toBe('library:jr');
    expect(jobsFromProgress([])).toEqual({});
  });

  it('fraction is clamped and empty jobs count as done', () => {
    expect(jobFraction({ bytesDone: 5, bytesTotal: 10 })).toBe(0.5);
    expect(jobFraction({ bytesDone: 50, bytesTotal: 10 })).toBe(1);
    expect(jobFraction({ bytesDone: 0, bytesTotal: 0 })).toBe(1);
  });
});

describe('samples view', () => {
  it('samples_done marks reported entries cached and updates library status + cache size', () => {
    const v = applyDone(viewFromList(list), { entries: ['jr/a.sfz'] });
    const lib = v.groups[0]!.libraries[0]!;
    expect(lib.entries[0]).toMatchObject({ status: 'cached', cachedBytes: 10 * MB });
    expect(lib.entries[1]!.status).toBe('missing');
    expect(lib.status).toBe('partial');
    expect(lib.cachedBytes).toBe(10 * MB);
    expect(v.cacheBytes).toBe(10 * MB);
    const all = applyDone(v, { entries: ['jr/b.sfz'] });
    expect(all.groups[0]!.libraries[0]!.status).toBe('cached');
    expect(applyDone(v, { entries: [] })).toBe(v);
  });

  it('library status rules', () => {
    expect(libraryStatus([{ status: 'local' }, { status: 'missing' }])).toBe('local');
    expect(libraryStatus([{ status: 'cached' }, { status: 'cached' }])).toBe('cached');
    expect(libraryStatus([{ status: 'missing' }])).toBe('missing');
    expect(libraryStatus([{ status: 'cached' }, { status: 'missing' }])).toBe('partial');
  });

  it('totals', () => {
    expect(totals(list.groups)).toEqual({ bytes: 30 * MB, cachedBytes: 5 * MB, entries: 2, cachedEntries: 0, missingBytes: 25 * MB });
  });

  it('formats sizes and ETA in Russian units', () => {
    expect(formatBytes(512)).toBe('512 Б');
    expect(formatBytes(1536)).toBe('1,5 КБ');
    expect(formatBytes(1.83 * 1024 * MB)).toBe('1,8 ГБ');
    expect(formatBytes(150 * MB)).toBe('150 МБ');
    expect(formatEta({ bytesDone: 0, bytesTotal: 60 * MB, rate: MB })).toBe('~1 мин');
    expect(formatEta({ bytesDone: 0, bytesTotal: 10, rate: 0 })).toBe('');
  });
});
