// Sample library: group → library → entry, sizes, cache status, licence/attribution, download / cancel / delete,
// live progress from `samples_progress` (PROTOCOL.md "Cloud & samples").

import { useState } from 'react';
import { cloudActions, useCloud, useEngine } from '../../store';
import { canSend } from '../../platform/trust';
import type { SampleEntry, SampleJob, SampleLibrary as Lib, SampleScope, SampleStatus } from '../../protocol/types';
import {
  ALL_JOB,
  formatBytes,
  formatEta,
  formatRate,
  jobForEntry,
  jobForLibrary,
  jobFraction,
  patchSampleEntries,
  totals,
} from '../../lib/samples';

const STATUS: Record<SampleStatus, string> = {
  local: 'локально',
  cached: 'скачано',
  partial: 'частично',
  missing: 'не скачано',
};

function Progress({ job, compact }: { job: SampleJob; compact?: boolean }) {
  const f = jobFraction(job);
  return (
    <div className={`sl-progress ${compact ? 'compact' : ''}`} role="progressbar" aria-valuenow={Math.round(f * 100)} aria-valuemin={0} aria-valuemax={100}>
      <div className="sl-bar">
        <i style={{ transform: `scaleX(${f.toFixed(3)})` }} className={job.state === 'queued' ? 'queued' : ''} />
      </div>
      {!compact && (
        <span className="sl-progress-text mono">
          {job.state === 'queued'
            ? 'в очереди'
            : `${Math.round(f * 100)}% · ${formatBytes(job.bytesDone)} / ${formatBytes(job.bytesTotal)} ${formatRate(job.rate)} ${formatEta(job)}`}
        </span>
      )}
    </div>
  );
}

function Actions({
  status,
  job,
  scope,
  target,
  bytes,
  cachedBytes,
  linked,
  inUse,
}: {
  status: SampleStatus;
  job: SampleJob | null;
  scope: SampleScope;
  target: string;
  bytes: number;
  cachedBytes: number;
  linked: boolean;
  inUse: boolean;
}) {
  const a = cloudActions();
  const canDelete = useCloud((s) => canSend(s.trust, 'samples_delete'));
  if (status === 'local') return <span className="hint">из assets/</span>;
  const own = job && job.id === (scope === 'entry' ? `entry:${target}` : `library:${target}`);
  return (
    <div className="sl-actions">
      {job ? (
        own ? (
          <button type="button" className="btn small ghost" onClick={() => void a.cancelSamples(job.id)} title="Остановить загрузку (скачанные файлы останутся)">
            Отмена
          </button>
        ) : null
      ) : (
        status !== 'cached' && (
          <button
            type="button"
            className="btn small"
            disabled={!linked}
            title={linked ? `Скачать ${formatBytes(bytes - cachedBytes)}` : 'Сначала привяжите движок к аккаунту'}
            onClick={() => void a.fetchSamples(scope, target)}
          >
            Скачать
          </button>
        )
      )}
      {cachedBytes > 0 && !job && canDelete && (
        <button
          type="button"
          className="icon-btn"
          disabled={inUse}
          title={inUse ? 'Используется текущим патчем' : `Удалить с диска (${formatBytes(cachedBytes)})`}
          aria-label="Удалить"
          onClick={() => {
            if (window.confirm(`Удалить скачанные сэмплы (${formatBytes(cachedBytes)})? Их можно будет скачать снова.`))
              void a.deleteSamples(scope, target);
          }}
        >
          <svg viewBox="0 0 16 16" width="13" height="13" aria-hidden>
            <path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.7 8.5h5.6l.7-8.5" fill="none" stroke="currentColor" strokeWidth="1.3" />
          </svg>
        </button>
      )}
    </div>
  );
}

function EntryRow({ e, jobs, linked, inUse }: { e: SampleEntry; jobs: Record<string, SampleJob>; linked: boolean; inUse: boolean }) {
  const job = jobForEntry(jobs, e.id);
  return (
    <li className={`sl-entry st-${e.status}`}>
      <div className="sl-entry-main">
        <span className="sl-entry-name" title={e.sfz}>
          {e.name}
          {inUse && <span className="pill accent">в патче</span>}
        </span>
        <span className="sl-size mono">
          {e.status === 'partial' ? `${formatBytes(e.cachedBytes)} / ` : ''}
          {formatBytes(e.bytes)}
        </span>
        <span className={`sl-status st-${e.status}`}>{STATUS[e.status]}</span>
        <Actions status={e.status} job={job} scope="entry" target={e.id} bytes={e.bytes} cachedBytes={e.cachedBytes} linked={linked} inUse={inUse} />
      </div>
      {job && job.id === `entry:${e.id}` && <Progress job={job} />}
      {job && job.id !== `entry:${e.id}` && <Progress job={job} compact />}
    </li>
  );
}

function LibraryRow({ lib, jobs, linked, used }: { lib: Lib; jobs: Record<string, SampleJob>; linked: boolean; used: string[] }) {
  const [open, setOpen] = useState(false);
  const job = jobForLibrary(jobs, lib.id);
  const libJobOwn = job?.id === `library:${lib.id}`;
  const inUse = lib.entries.some((e) => used.includes(e.id));
  return (
    <li className={`sl-lib st-${lib.status}`}>
      <div className="sl-lib-main">
        <button type="button" className={`sl-toggle ${open ? 'open' : ''}`} aria-expanded={open} onClick={() => setOpen((v) => !v)}>
          <svg viewBox="0 0 10 10" width="9" height="9" aria-hidden>
            <path d="M3 2l4 3-4 3" fill="none" stroke="currentColor" strokeWidth="1.4" />
          </svg>
          <span className="sl-lib-name">{lib.name}</span>
        </button>
        <span className="chip">{lib.licence}</span>
        <span className="sl-size mono">
          {lib.cachedBytes > 0 && lib.status !== 'cached' && lib.status !== 'local' ? `${formatBytes(lib.cachedBytes)} / ` : ''}
          {formatBytes(lib.bytes)}
        </span>
        <span className={`sl-status st-${lib.status}`}>{STATUS[lib.status]}</span>
        <Actions status={lib.status} job={job} scope="library" target={lib.id} bytes={lib.bytes} cachedBytes={lib.cachedBytes} linked={linked} inUse={inUse} />
      </div>
      {job && libJobOwn && <Progress job={job} />}
      {open && (
        <div className="sl-lib-body">
          <p className="sl-attr">
            {lib.author && (
              <>
                Автор: <b>{lib.author}</b> ·{' '}
              </>
            )}
            Лицензия: <b>{lib.licence}</b>
            {lib.attribution && <span className="sl-attr-text">{lib.attribution}</span>}
          </p>
          <ul className="sl-entries">
            {lib.entries.map((e) => (
              <EntryRow key={e.id} e={e} jobs={jobs} linked={linked} inUse={used.includes(e.id)} />
            ))}
          </ul>
        </div>
      )}
    </li>
  );
}

export function SampleLibrary() {
  const samples = useCloud((s) => s.samples);
  const jobs = useCloud((s) => s.jobs);
  const loading = useCloud((s) => s.samplesLoading);
  const error = useCloud((s) => s.samplesError);
  const linked = useCloud((s) => !!s.cloud?.linked);
  const patch = useEngine((s) => s.patch);
  const used = patchSampleEntries(patch);
  const a = cloudActions();
  const t = samples ? totals(samples.groups) : null;
  const allJob = jobs[ALL_JOB];
  const anyJob = Object.keys(jobs).length > 0;

  return (
    <section className="sample-lib">
      <div className="cloud-card-head">
        <span className="panel-label">Библиотека сэмплов</span>
        <div className="spacer" />
        <button type="button" className="icon-btn" onClick={() => void a.refreshSamples()} title="Обновить каталог" disabled={loading}>
          ↻
        </button>
      </div>

      {samples && (
        <div className="sl-summary">
          <div>
            <span className="stat-k">В КЭШЕ</span> <b className="mono">{formatBytes(samples.cacheBytes)}</b>
          </div>
          <div>
            <span className="stat-k">СВОБОДНО</span> <span className="mono">{formatBytes(samples.freeBytes)}</span>
          </div>
          {t && (
            <div>
              <span className="stat-k">ИНСТРУМЕНТЫ</span>{' '}
              <span className="mono">
                {t.cachedEntries} / {t.entries}
              </span>
            </div>
          )}
          <div className="spacer" />
          {anyJob ? (
            <button type="button" className="btn small ghost" onClick={() => void a.cancelSamples()}>
              Отменить все
            </button>
          ) : (
            t &&
            t.missingBytes > 0 && (
              <button type="button" className="btn small" disabled={!linked} onClick={() => void a.fetchSamples('all')}>
                Скачать всё ({formatBytes(t.missingBytes)})
              </button>
            )
          )}
        </div>
      )}
      {allJob && <Progress job={allJob} />}

      {!linked && (
        <p className="note">Скачивание — после привязки движка к аккаунту.</p>
      )}
      {samples?.manifest.state === 'stale' && linked && <p className="note warn">Каталог из кэша: сервер недоступен.</p>}
      {error && (
        <p className="note bad" role="alert">
          {error}
        </p>
      )}

      {!samples && <p className="muted">{loading ? 'Загружаю каталог…' : 'Каталог ещё не загружен.'}</p>}
      {samples && samples.groups.length === 0 && (
        <p className="muted">{samples.manifest.state === 'none' ? 'Каталог — после привязки к аккаунту.' : 'Каталог пуст.'}</p>
      )}

      {samples?.groups.map((g) => (
        <div key={g.id} className="sl-group">
          <div className="sl-group-head">{g.name}</div>
          <ul className="sl-libs">
            {g.libraries.map((lib) => (
              <LibraryRow key={lib.id} lib={lib} jobs={jobs} linked={linked} used={used} />
            ))}
          </ul>
        </div>
      ))}
    </section>
  );
}
