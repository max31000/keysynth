// Cloud / platform state: engine discovery phase, hello info (version, trust, cloud link), sync status, sample
// library + download jobs, update check and the automatic account link (PROTOCOL.md "Cloud & samples").
// Separate from engineStore: it only listens to the same EngineClient.

import { createStore, type StoreApi } from 'zustand/vanilla';
import type { EngineClient } from '../protocol/client';
import type {
  CloudStatus,
  EngineMsg,
  SampleScope,
  SessionTrust,
  SyncStatusEvent,
} from '../protocol/types';
import {
  applyDone,
  findEntry,
  jobsFromProgress,
  viewFromList,
  type JobMap,
  type SamplesView,
} from '../lib/samples';
import { initialDiscovery, type DiscoveryState } from '../platform/discovery';
import {
  defaultDeviceName,
  linkEngine,
  linkErrorText,
  ownerFromDevices,
  planLink,
  type DeviceRow,
  type LinkPlan,
  type Minted,
  type Owner,
} from '../platform/link';
import { isOutdated, type LatestRelease } from '../platform/version';
import type { AppMode } from '../platform/mode';

export type MeStatus = 'loading' | 'unavailable' | 'anonymous' | 'authenticated';
export interface MeInfo {
  status: MeStatus;
  granted: boolean;
  name: string | null;
}

export interface SampleNotice {
  kind: 'done' | 'error' | 'cancelled';
  text: string;
  /** job id it belongs to */
  job: string;
  at: number;
}

export interface LinkUi {
  busy: boolean;
  /** last failure (Russian text) */
  error: string | null;
  owner: Owner;
  /** account-mismatch confirmation pending: name of the account the engine is linked to now */
  confirm: { current: string | null } | null;
  /** first link on this PC: local presets/patterns copied into the account profile */
  merged: { presets: number; patterns: number } | null;
  /** the user unlinked explicitly: no automatic re-link until «Привязать» */
  held: boolean;
}

export interface CloudState {
  mode: AppMode;
  discovery: DiscoveryState;
  /** engine semver from `hello` (null = old engine without the field / not connected yet) */
  engineVersion: string | null;
  trust: SessionTrust | null;
  cloud: CloudStatus | null;
  sync: SyncStatusEvent | null;
  samples: SamplesView | null;
  samplesLoading: boolean;
  samplesError: string | null;
  jobs: JobMap;
  sampleNotice: SampleNotice | null;
  latest: LatestRelease | null;
  me: MeInfo;
  link: LinkUi;

  handle(msg: EngineMsg): void;
  setDiscovery(d: DiscoveryState): void;
  setLatest(l: LatestRelease | null): void;
  /** current platform user (from the kit's useMe) → re-plans the link */
  setMe(me: MeInfo): void;
  plan(): LinkPlan;
  outdated(): boolean;
  /** evaluate the plan and act (mint/link/check/remint); safe to call often */
  reconcile(): Promise<void>;
  /** user confirmed «привязать к моему аккаунту» (account mismatch) */
  confirmReplace(): Promise<void>;
  dismissConfirm(): void;
  retryLink(): Promise<void>;
  unlink(): Promise<void>;
  syncNow(): Promise<void>;
  refreshSamples(): Promise<void>;
  fetchSamples(scope: SampleScope, target?: string): Promise<void>;
  cancelSamples(job?: string): Promise<void>;
  deleteSamples(scope: SampleScope, target?: string): Promise<boolean>;
  dismissSampleNotice(): void;
}

export interface CloudDeps {
  client: Pick<EngineClient, 'request' | 'on' | 'onStatus' | 'status'>;
  mode: AppMode;
  api: {
    mint(deviceId: string, name: string): Promise<Minted>;
    listDevices(): Promise<DeviceRow[]>;
  };
  deviceName?: string;
  /** remembers an explicit unlink across reloads */
  storage?: Pick<Storage, 'getItem' | 'setItem' | 'removeItem'> | null;
  now?: () => number;
  /** `samples_done` → re-list after this delay (coalesces bursts) */
  relistDelayMs?: number;
}

export type CloudStore = StoreApi<CloudState>;

const errText = (e: unknown) => (e instanceof Error ? e.message : String(e));

export function createCloudStore(deps: CloudDeps): CloudStore {
  const { client } = deps;
  const now = deps.now ?? (() => Date.now());
  const deviceName = deps.deviceName ?? defaultDeviceName(typeof navigator !== 'undefined' ? navigator.platform : undefined);
  const relistDelay = deps.relistDelayMs ?? 300;
  let relistTimer: ReturnType<typeof setTimeout> | null = null;
  /** plan keys that already failed: not retried automatically until the inputs change */
  let failedKey: string | null = null;
  let reconciling: Promise<void> | null = null;
  /** after an explicit unlink the page does not re-link on its own until the user asks (persisted per browser) */
  const HOLD_KEY = 'keysynth.linkHold';
  let manualHold = (() => {
    try {
      return deps.storage?.getItem(HOLD_KEY) === '1';
    } catch {
      return false;
    }
  })();
  const setHold = (v: boolean) => {
    manualHold = v;
    store.setState({ link: { ...store.getState().link, held: v } });
    try {
      if (v) deps.storage?.setItem(HOLD_KEY, '1');
      else deps.storage?.removeItem(HOLD_KEY);
    } catch {
      /* private mode */
    }
  };

  const store = createStore<CloudState>()((set, get) => {
    const scheduleRelist = () => {
      if (relistTimer) return;
      relistTimer = setTimeout(() => {
        relistTimer = null;
        void get().refreshSamples();
      }, relistDelay);
    };

    const sendLink = async (token: string, replace: boolean) => {
      const msg = replace ? { type: 'cloud_link' as const, token, replace: true } : { type: 'cloud_link' as const, token };
      return client.request(msg, { expect: 'cloud_status', timeoutMs: 20000 });
    };

    const doLink = async (replace: boolean) => {
      const cloud = get().cloud;
      if (!cloud) return;
      set({ link: { ...get().link, busy: true, error: null } });
      const r = await linkEngine({ mint: deps.api.mint, sendLink }, cloud.deviceId, deviceName, replace);
      if (r.ok) {
        failedKey = null;
        set({
          cloud: stripStatus(r.status),
          link: { busy: false, error: null, owner: 'mine', confirm: null, merged: r.merged ?? get().link.merged, held: false },
        });
        return;
      }
      if (r.code === 'account_mismatch') {
        set({ link: { ...get().link, busy: false, confirm: { current: get().cloud?.accountName ?? null } } });
        return;
      }
      failedKey = planKey();
      set({ link: { ...get().link, busy: false, error: linkErrorText(r.code) } });
    };

    const planKey = () => {
      const c = get().cloud;
      return `${get().plan().kind}|${c?.deviceId}|${c?.linked}|${c?.accountId}|${c?.expiresAt}|${get().me.status}`;
    };

    return {
      mode: deps.mode,
      discovery: initialDiscovery(false),
      engineVersion: null,
      trust: null,
      cloud: null,
      sync: null,
      samples: null,
      samplesLoading: false,
      samplesError: null,
      jobs: {},
      sampleNotice: null,
      latest: null,
      me: { status: 'loading', granted: false, name: null },
      link: { busy: false, error: null, owner: 'unknown', confirm: null, merged: null, held: manualHold },

      handle(msg) {
        switch (msg.type) {
          case 'state':
            // only the reply to `hello` carries these (PROTOCOL.md "Hello additions")
            if (msg.cloud || msg.version !== undefined || msg.trust) {
              const prev = get().cloud;
              set({
                engineVersion: msg.version ?? null,
                trust: msg.trust ?? null,
                cloud: msg.cloud ?? null,
                ...(msg.cloud && prev && prev.accountId !== msg.cloud.accountId ? { link: { ...get().link, owner: 'unknown' as Owner } } : {}),
              });
              void get().reconcile();
            }
            break;
          case 'cloud_status': {
            const status = stripStatus(msg);
            const merged = msg.merged;
            const prev = get().cloud;
            const accountChanged = !prev || prev.accountId !== status.accountId || prev.expiresAt !== status.expiresAt;
            set({
              cloud: status,
              ...(merged ? { link: { ...get().link, merged } } : {}),
            });
            if (accountChanged && get().link.owner !== 'mine') set({ link: { ...get().link, owner: 'unknown' } });
            if (!status.linked) set({ link: { ...get().link, owner: 'unknown' }, jobs: {} });
            if (prev?.linked !== status.linked) scheduleRelist();
            void get().reconcile();
            break;
          }
          case 'sync_status':
            set({ sync: msg });
            break;
          case 'samples_list_ok':
            set({ samples: viewFromList(msg), samplesLoading: false, samplesError: null });
            break;
          case 'samples_progress':
            set({ jobs: jobsFromProgress(msg.jobs) });
            break;
          case 'samples_done': {
            const jobs = { ...get().jobs };
            delete jobs[msg.job];
            const s = get().samples;
            const name = msg.scope === 'entry' && s ? findEntry(s.groups, msg.target)?.entry.name : undefined;
            set({
              jobs,
              samples: s ? applyDone(s, msg) : s,
              sampleNotice: {
                kind: msg.cancelled ? 'cancelled' : 'done',
                text: msg.cancelled ? 'Загрузка отменена' : name ? `Сэмплы «${name}» загружены` : 'Сэмплы загружены',
                job: msg.job,
                at: now(),
              },
            });
            scheduleRelist();
            break;
          }
          case 'samples_error': {
            const jobs = { ...get().jobs };
            delete jobs[msg.job];
            set({
              jobs,
              sampleNotice: { kind: 'error', text: samplesErrorText(msg.code, msg.message), job: msg.job, at: now() },
            });
            scheduleRelist();
            break;
          }
          default:
            break;
        }
      },

      setDiscovery(d) {
        const was = get().discovery.phase;
        set({ discovery: d });
        if (was === 'connected' && d.phase !== 'connected') set({ jobs: {} });
      },
      setLatest: (latest) => set({ latest }),
      setMe(me) {
        const prev = get().me;
        if (prev.status === me.status && prev.granted === me.granted && prev.name === me.name) return;
        set({ me, link: { ...get().link, owner: 'unknown' } });
        failedKey = null;
        void get().reconcile();
      },

      plan() {
        const s = get();
        return planLink({
          mode: s.mode,
          connected: s.discovery.phase === 'connected' || client.status === 'open',
          cloud: s.cloud,
          meStatus: s.me.status,
          granted: s.me.granted,
          owner: s.link.owner,
          now: now(),
        });
      },

      outdated: () => isOutdated(get().engineVersion, get().latest),

      reconcile() {
        if (reconciling) return reconciling;
        const run = (async () => {
          await Promise.resolve(); // let `reconciling` be assigned before the body (and its finally) runs
          try {
            // loop: one step may lead to the next (check-owner → remint)
            for (let i = 0; i < 3; i++) {
              const p = get().plan();
              if (get().link.busy || get().link.confirm) return;
              const key = planKey();
              if (key === failedKey) return;
              if (manualHold && p.kind === 'link') return;
              if (p.kind === 'link' || p.kind === 'remint') {
                await doLink(false);
              } else if (p.kind === 'check-owner') {
                const cloud = get().cloud!;
                set({ link: { ...get().link, busy: true } });
                try {
                  const items = await deps.api.listDevices();
                  set({ link: { ...get().link, busy: false, owner: ownerFromDevices(cloud, items) } });
                } catch (e) {
                  failedKey = key;
                  set({ link: { ...get().link, busy: false, error: linkErrorText(codeOf(e)) } });
                  return;
                }
              } else return;
            }
          } finally {
            reconciling = null;
          }
        })();
        reconciling = run;
        return run;
      },

      async confirmReplace() {
        set({ link: { ...get().link, confirm: null } });
        await doLink(true);
      },
      dismissConfirm: () => set({ link: { ...get().link, confirm: null } }),
      async retryLink() {
        failedKey = null;
        setHold(false);
        set({ link: { ...get().link, error: null, owner: get().link.owner === 'foreign' ? 'foreign' : 'unknown' } });
        if (get().plan().kind === 'foreign') {
          // known to be another account's link: ask first, mint only after the user's yes
          set({ link: { ...get().link, confirm: { current: get().cloud?.accountName ?? null } } });
          return;
        }
        await get().reconcile();
      },

      async unlink() {
        setHold(true); // before the request: its cloud_status reply must not trigger an automatic re-link
        try {
          set({ link: { ...get().link, busy: true, error: null } });
          const r = await client.request({ type: 'cloud_unlink' }, { expect: 'cloud_status', timeoutMs: 15000 });
          // stay unlinked until the user links again (no automatic re-link right after an explicit unlink)
          failedKey = null;
          set({ cloud: stripStatus(r), link: { busy: false, error: null, owner: 'unknown', confirm: null, merged: null, held: true }, jobs: {} });
        } catch (e) {
          setHold(false);
          set({ link: { ...get().link, busy: false, error: errText(e) } });
        }
      },

      async syncNow() {
        try {
          await client.request({ type: 'sync_now' }, { expect: 'sync_now_ok' });
        } catch (e) {
          set({ link: { ...get().link, error: errText(e) } });
        }
      },

      async refreshSamples() {
        if (client.status !== 'open') return;
        set({ samplesLoading: true });
        try {
          // the engine revalidates the manifest first (≤ 5 s)
          await client.request({ type: 'samples_list' }, { expect: 'samples_list_ok', timeoutMs: 15000 });
        } catch (e) {
          set({ samplesLoading: false, samplesError: errText(e) });
        }
      },

      async fetchSamples(scope, target) {
        try {
          await client.request(target !== undefined ? { type: 'samples_fetch', scope, target } : { type: 'samples_fetch', scope }, {
            expect: 'samples_fetch_ok',
          });
        } catch (e) {
          set({ samplesError: samplesErrorText(codeOf(e), errText(e)) });
        }
      },

      async cancelSamples(job) {
        try {
          await client.request(job ? { type: 'samples_cancel', job } : { type: 'samples_cancel' }, { expect: 'samples_cancel_ok' });
        } catch (e) {
          set({ samplesError: errText(e) });
        }
      },

      async deleteSamples(scope, target) {
        try {
          await client.request(target !== undefined ? { type: 'samples_delete', scope, target } : { type: 'samples_delete', scope }, {
            expect: 'samples_delete_ok',
            timeoutMs: 30000,
          });
          await get().refreshSamples();
          return true;
        } catch (e) {
          set({ samplesError: samplesErrorText(codeOf(e), errText(e)) });
          return false;
        }
      },

      dismissSampleNotice: () => set({ sampleNotice: null }),
    };
  });

  client.on('*', (m) => store.getState().handle(m as EngineMsg));
  return store;
}

function stripStatus(m: CloudStatus & { type?: string; id?: number; merged?: unknown }): CloudStatus {
  const { linked, accountId, accountName, deviceId, expiresAt, online, error } = m;
  return { linked, accountId, accountName, deviceId, expiresAt, online, error };
}

function codeOf(e: unknown): string {
  const o = e as { code?: unknown; status?: unknown } | null;
  if (typeof o?.code === 'string' && o.code) return o.code;
  if (typeof o?.status === 'number') return `http_${o.status}`;
  return 'error';
}

export function samplesErrorText(code: string, message: string): string {
  switch (code) {
    case 'not_linked':
      return 'Движок не привязан к аккаунту — сэмплы скачиваются только после привязки.';
    case 'offline':
      return 'Нет связи с mvv42.ru — загрузка остановлена.';
    case 'unauthorized':
      return 'Ключ устройства отозван — откройте страницу на mvv42.ru, чтобы привязать движок снова.';
    case 'disk_full':
      return 'На диске закончилось место.';
    case 'checksum_mismatch':
      return 'Файл повреждён при загрузке (контрольная сумма не совпала).';
    case 'in_use':
      return 'Эти сэмплы используются текущим патчем — сначала загрузите другой пресет.';
    case 'read_only':
      return 'Локальная библиотека (из assets/) — её нельзя удалить отсюда.';
    case 'not_found':
      return 'Сэмплы не найдены в каталоге.';
    default:
      return message || code;
  }
}

/** Is the engine linked and ready for cloud features? */
export const isLinked = (s: CloudState) => !!s.cloud?.linked;
