// Sample library view model (PROTOCOL.md "Cloud & samples": samples_list_ok / samples_progress / samples_done).
// Pure functions; the cloud store keeps the last `samples_list_ok` and the live job map.

import type {
  Patch,
  SampleEntry,
  SampleGroup,
  SampleJob,
  SampleLibrary,
  SamplesDoneEvent,
  SamplesListOk,
  SampleStatus,
} from '../protocol/types';

export const SAMPLES_PREFIX = 'assets/samples/';

export interface SamplesView {
  groups: SampleGroup[];
  cacheBytes: number;
  freeBytes: number;
  manifest: SamplesListOk['manifest'];
}

export type JobMap = Record<string, SampleJob>;

export function viewFromList(m: SamplesListOk): SamplesView {
  return { groups: m.groups, cacheBytes: m.cacheBytes, freeBytes: m.freeBytes, manifest: m.manifest };
}

/** `samples_progress` lists every queued/running job: it replaces the map. */
export function jobsFromProgress(jobs: SampleJob[]): JobMap {
  const out: JobMap = {};
  for (const j of jobs) out[j.id] = j;
  return out;
}

export const entryJobId = (entryId: string) => `entry:${entryId}`;
export const libraryJobId = (libId: string) => `library:${libId}`;
export const ALL_JOB = 'all';

/** Entry id of a sampler `state.sfz` (`assets/samples/<entry id>`), else null. */
export function entryIdFromSfz(sfz: unknown): string | null {
  if (typeof sfz !== 'string' || !sfz.startsWith(SAMPLES_PREFIX) || sfz.length === SAMPLES_PREFIX.length) return null;
  return sfz.slice(SAMPLES_PREFIX.length);
}

/** Entry ids the patch's samplers use (instrument `state.sfz`), in layer order, unique. */
export function patchSampleEntries(patch: Patch | null): string[] {
  if (!patch) return [];
  const out: string[] = [];
  for (const l of patch.layers) {
    const id = entryIdFromSfz((l.instrument.state as { sfz?: unknown } | undefined)?.sfz);
    if (id && !out.includes(id)) out.push(id);
  }
  return out;
}

export const libraryOfEntry = (entryId: string) => entryId.split('/')[0] ?? '';

/** The job currently covering an entry: its own, its library's, or `all` (most specific first). */
export function jobForEntry(jobs: JobMap, entryId: string): SampleJob | null {
  return jobs[entryJobId(entryId)] ?? jobs[libraryJobId(libraryOfEntry(entryId))] ?? jobs[ALL_JOB] ?? null;
}

export function jobForLibrary(jobs: JobMap, libId: string): SampleJob | null {
  return jobs[libraryJobId(libId)] ?? jobs[ALL_JOB] ?? null;
}

/** 0..1, 1 for an empty job (nothing to download). */
export function jobFraction(j: Pick<SampleJob, 'bytesDone' | 'bytesTotal'>): number {
  return j.bytesTotal > 0 ? Math.min(1, Math.max(0, j.bytesDone / j.bytesTotal)) : 1;
}

export function findEntry(groups: SampleGroup[], entryId: string): { lib: SampleLibrary; entry: SampleEntry } | null {
  for (const g of groups)
    for (const lib of g.libraries) {
      const entry = lib.entries.find((e) => e.id === entryId);
      if (entry) return { lib, entry };
    }
  return null;
}

/** Library status from its entries (PROTOCOL.md: local if any is local, cached if all, missing if none). */
export function libraryStatus(entries: Pick<SampleEntry, 'status'>[]): SampleStatus {
  if (entries.some((e) => e.status === 'local')) return 'local';
  if (entries.length && entries.every((e) => e.status === 'cached')) return 'cached';
  if (entries.every((e) => e.status === 'missing')) return 'missing';
  return 'partial';
}

/**
 * Optimistic update on `samples_done`: entries it reports as fully cached become `cached` (bytes counted), the
 * library status/cachedBytes follow. Shared-file accounting is the engine's job; the next samples_list corrects it.
 */
export function applyDone(view: SamplesView, done: Pick<SamplesDoneEvent, 'entries'>): SamplesView {
  if (!done.entries.length) return view;
  const set = new Set(done.entries);
  let added = 0;
  const groups = view.groups.map((g) => ({
    ...g,
    libraries: g.libraries.map((lib) => {
      if (!lib.entries.some((e) => set.has(e.id))) return lib;
      const entries = lib.entries.map((e) => {
        if (!set.has(e.id) || e.status === 'cached' || e.status === 'local') return e;
        added += e.bytes - e.cachedBytes;
        return { ...e, status: 'cached' as const, cachedBytes: e.bytes };
      });
      const cachedBytes = Math.min(lib.bytes, entries.reduce((s, e) => s + (e.status === 'local' ? 0 : e.cachedBytes), 0));
      return { ...lib, entries, status: libraryStatus(entries), cachedBytes: Math.max(lib.cachedBytes, cachedBytes) };
    }),
  }));
  return { ...view, groups, cacheBytes: view.cacheBytes + added };
}

export interface SamplesTotals {
  bytes: number;
  cachedBytes: number;
  entries: number;
  cachedEntries: number;
  missingBytes: number;
}

export function totals(groups: SampleGroup[]): SamplesTotals {
  const t: SamplesTotals = { bytes: 0, cachedBytes: 0, entries: 0, cachedEntries: 0, missingBytes: 0 };
  for (const g of groups)
    for (const lib of g.libraries) {
      t.bytes += lib.bytes;
      t.cachedBytes += lib.cachedBytes;
      for (const e of lib.entries) {
        t.entries++;
        if (e.status === 'cached' || e.status === 'local') t.cachedEntries++;
      }
      if (lib.status !== 'local') t.missingBytes += Math.max(0, lib.bytes - lib.cachedBytes);
    }
  return t;
}

const UNITS = ['Б', 'КБ', 'МБ', 'ГБ', 'ТБ'];

/** 1536 → "1,5 КБ"; binary units, one decimal under 100. */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) return '—';
  let v = n;
  let u = 0;
  while (v >= 1024 && u < UNITS.length - 1) {
    v /= 1024;
    u++;
  }
  const s = u === 0 || v >= 100 ? String(Math.round(v)) : v.toFixed(1).replace('.', ',');
  return `${s} ${UNITS[u]}`;
}

export function formatRate(bytesPerSec: number): string {
  return bytesPerSec > 0 ? `${formatBytes(bytesPerSec)}/с` : '';
}

/** Remaining time "~2 мин" from a job's rate; '' while unknown. */
export function formatEta(j: Pick<SampleJob, 'bytesDone' | 'bytesTotal' | 'rate'>): string {
  if (j.rate <= 0 || j.bytesTotal <= j.bytesDone) return '';
  const s = (j.bytesTotal - j.bytesDone) / j.rate;
  if (s < 60) return `~${Math.max(1, Math.round(s))} с`;
  if (s < 3600) return `~${Math.round(s / 60)} мин`;
  return `~${(s / 3600).toFixed(1).replace('.', ',')} ч`;
}
