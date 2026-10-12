// Sampler whose samples are not downloaded (engine readout `samples_missing`, PROTOCOL.md "Auto-fetch on patch
// load"): a notice on the module panel instead of an error, with the action that fixes it.

import { useEffect, useState } from 'react';
import { cloudActions, useCloud, useEngine } from '../store';
import { findModule } from '../store/patchOps';
import { live, onFrame } from '../store/liveBus';
import { entryIdFromSfz, findEntry, formatBytes, jobForEntry } from '../lib/samples';
import { MODE } from '../platform/mode';

/** 0 = present, 1 = not linked, 2 = server unreachable (the engine retries) */
function useMissingReadout(node: number): number {
  const [v, setV] = useState(0);
  useEffect(
    () =>
      onFrame(() => {
        const r = live.telemetry?.readouts?.[String(node)]?.samples_missing;
        const n = r === undefined ? 0 : Math.round(r);
        setV((prev) => (prev === n ? prev : n));
      }),
    [node],
  );
  return v;
}

export function SamplesMissing({ node }: { node: number }) {
  const sfz = useEngine((s) => (findModule(s.patch, node)?.slot.state as { sfz?: unknown } | undefined)?.sfz);
  const entryId = entryIdFromSfz(sfz);
  const reason = useMissingReadout(node);
  const samples = useCloud((s) => s.samples);
  const jobs = useCloud((s) => s.jobs);
  const linked = useCloud((s) => !!s.cloud?.linked);
  if (!entryId || reason === 0) return null;
  const found = samples ? findEntry(samples.groups, entryId) : null;
  const job = jobForEntry(jobs, entryId);
  const left = found ? Math.max(0, found.entry.bytes - found.entry.cachedBytes) : 0;
  return (
    <div className="samples-missing" role="status">
      <span className="led warn" />
      <span>
        Сэмплы не скачаны{reason === 2 ? ': нет связи с сервером, повтор автоматически' : linked ? '' : ': движок не привязан к аккаунту'}
      </span>
      {linked && found && !job && (
        <button type="button" className="btn small primary" onClick={() => void cloudActions().fetchSamples('entry', entryId)}>
          Скачать сэмплы ({formatBytes(left)})
        </button>
      )}
      {!linked && MODE.mode === 'platform' && (
        <button type="button" className="btn small primary" onClick={() => void cloudActions().retryLink()}>
          Подключить
        </button>
      )}
    </div>
  );
}
