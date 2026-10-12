// Cloud overlays: account-mismatch confirmation, update banner, sample download toast (auto-fetch feedback).

import { useEffect, useRef, useState } from 'react';
import { useShallow } from 'zustand/react/shallow';
import { cloudActions, useCloud, useEngine } from '../../store';
import { findEntry, formatBytes, formatEta, formatRate, jobForEntry, jobFraction, patchSampleEntries } from '../../lib/samples';
import { DOWNLOAD_URL } from '../../platform/version';

/** PROTOCOL.md `cloud_link` step 3: re-linking to another account needs an explicit yes. */
export function LinkConfirm() {
  const confirm = useCloud((s) => s.link.confirm);
  const me = useCloud((s) => s.me.name);
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const d = ref.current;
    if (!d) return;
    if (confirm && !d.open) d.showModal();
    else if (!confirm && d.open) d.close();
  }, [confirm]);
  const a = cloudActions();
  return (
    <dialog ref={ref} className="dialog confirm" onCancel={() => a.dismissConfirm()} onClose={() => a.dismissConfirm()}>
      <header className="dialog-head">
        <h2>Движок привязан к другому аккаунту</h2>
      </header>
      <div className="confirm-body">
        <p>
          {confirm?.current && confirm.current === me ? (
            <>
              Привязан к <b>«{confirm.current}»</b>, но среди устройств вашего аккаунта его нет. Привязать к вашему?
            </>
          ) : (
            <>
              Привязан к <b>«{confirm?.current ?? 'другой'}»</b>. Привязать к{me ? <b> «{me}»</b> : ' вашему'}?
            </>
          )}
        </p>
        <p className="muted">Пресеты прежнего аккаунта останутся на ПК без синхронизации.</p>
        <div className="row gap end">
          <button type="button" className="btn" onClick={() => a.dismissConfirm()}>
            Оставить как есть
          </button>
          <button type="button" className="btn primary" onClick={() => void a.confirmReplace()}>
            Привязать к моему аккаунту
          </button>
        </div>
      </div>
    </dialog>
  );
}

export function UpdateBanner() {
  const { outdated, latest, version } = useCloud(
    useShallow((s) => ({ outdated: s.discovery.phase === 'connected' && s.outdated(), latest: s.latest, version: s.engineVersion })),
  );
  const [hidden, setHidden] = useState<string | null>(null);
  if (!outdated || !latest || hidden === latest.version) return null;
  return (
    <div className="update-banner" role="status">
      <span className="led on" />
      <span>
        Новая версия движка <b className="mono">{latest.version}</b>
        {version ? (
          <>
            {' '}
            (у вас <span className="mono">{version}</span>)
          </>
        ) : (
          ' (у вас версия без облака)'
        )}
        .
      </span>
      <a className="btn small primary" href={DOWNLOAD_URL} rel="noopener">
        Скачать
      </a>
      <a className="btn small ghost" href={latest.url} target="_blank" rel="noopener">
        Что нового
      </a>
      <button type="button" className="icon-btn" aria-label="Скрыть" onClick={() => setHidden(latest.version)}>
        ×
      </button>
    </div>
  );
}

/**
 * Feedback for downloads the current patch waits for (auto-fetch on preset load, PROTOCOL.md "Auto-fetch") plus
 * the end-of-job notice (done / cancelled / error).
 */
export function SampleToast() {
  const patch = useEngine((s) => s.patch);
  const jobs = useCloud((s) => s.jobs);
  const samples = useCloud((s) => s.samples);
  const notice = useCloud((s) => s.sampleNotice);
  const a = cloudActions();

  const entries = patchSampleEntries(patch);
  const waiting = entries.map((id) => ({ id, job: jobForEntry(jobs, id) })).find((x) => x.job);
  const unknownEntry = !!waiting && !!samples && !findEntry(samples.groups, waiting.id);

  // name lookup needs the catalogue: fetch it once when an auto-fetch starts before the drawer was opened
  useEffect(() => {
    if (waiting && (!samples || unknownEntry)) void a.refreshSamples();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [waiting?.job?.id]);

  useEffect(() => {
    if (!notice || notice.kind === 'error') return;
    const t = setTimeout(() => a.dismissSampleNotice(), 4000);
    return () => clearTimeout(t);
  }, [notice, a]);

  if (waiting?.job) {
    const j = waiting.job;
    const name = (samples && findEntry(samples.groups, waiting.id)?.entry.name) ?? waiting.id.split('/').pop()?.replace(/\.sfz$/i, '');
    const f = jobFraction(j);
    return (
      <div className="sample-toast" role="status" aria-live="polite">
        <div className="sample-toast-head">
          <span className="led blink" />
          <span>
            Загружаю сэмплы <b>«{name}»</b>
          </span>
          <span className="mono muted">{j.state === 'queued' ? 'в очереди' : `${Math.round(f * 100)}%`}</span>
          <button type="button" className="plugin-toast-btn" onClick={() => void a.cancelSamples(j.id)}>
            Отмена
          </button>
        </div>
        <div className="sl-bar">
          <i style={{ transform: `scaleX(${f.toFixed(3)})` }} />
        </div>
        <div className="hint mono">
          {formatBytes(j.bytesDone)} / {formatBytes(j.bytesTotal)} {formatRate(j.rate)} {formatEta(j)}
        </div>
      </div>
    );
  }
  if (!notice) return null;
  return (
    <div className={`sample-toast kind-${notice.kind}`} role={notice.kind === 'error' ? 'alert' : 'status'}>
      <div className="sample-toast-head">
        <span className={`led ${notice.kind === 'done' ? 'on' : ''}`} />
        <span>{notice.text}</span>
        <button type="button" className="plugin-toast-btn close" aria-label="Закрыть" onClick={() => a.dismissSampleNotice()}>
          ×
        </button>
      </div>
    </div>
  );
}
