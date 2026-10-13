// `chord` MIDI processor: «Learn» (PROTOCOL.md `chord_learn`: the notes held now become the chord memory), the
// learned memory and the notes currently held on its input (telemetry readouts).

import { useEffect, useState } from 'react';
import { actions, useEngine } from '../store';
import { findModule } from '../store/patchOps';
import { live, onFrame } from '../store/liveBus';
import { heldNotes, intervalNames, memoryIntervals } from '../lib/chord';
import { noteName } from '../lib/notes';

const ERROR_MS = 2500;

function useHeld(node: number): number[] {
  const [held, setHeld] = useState<number[]>([]);
  useEffect(
    () =>
      onFrame(() => {
        const next = heldNotes(live.telemetry?.readouts?.[String(node)]);
        setHeld((prev) => (prev.length === next.length && prev.every((n, i) => n === next[i]) ? prev : next));
      }),
    [node],
  );
  return held;
}

const errorText = (code: string) =>
  code === 'no_notes' ? 'Hold 2 or more notes' : code === 'not_found' ? 'Module not found' : `Error: ${code}`;

export function ChordLearn({ node }: { node: number }) {
  const params = useEngine((s) => findModule(s.patch, node)?.slot.params);
  const mem = memoryIntervals(params);
  const held = useHeld(node);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!error) return;
    const t = setTimeout(() => setError(null), ERROR_MS);
    return () => clearTimeout(t);
  }, [error]);

  const learn = async () => {
    setBusy(true);
    const code = await actions().chordLearn(node);
    setBusy(false);
    setError(code ? errorText(code) : null);
  };

  return (
    <div className="chord-learn">
      <button type="button" className="btn small primary" disabled={busy} onClick={() => void learn()}>
        Learn
      </button>
      <span className="chord-mem">
        <span className="chord-k">Memory</span>
        {mem.length ? (
          <>
            <span className="mono">{mem.join(' · ')}</span>
            <span className="muted">({intervalNames(mem)})</span>
          </>
        ) : (
          <span className="muted">—</span>
        )}
      </span>
      <span className="chord-held">
        <span className="chord-k">Held</span>
        <span className="mono">{held.length ? held.map(noteName).join(' ') : '—'}</span>
      </span>
      {error && (
        <span className="chord-error" role="alert">
          {error}
        </span>
      )}
    </div>
  );
}
