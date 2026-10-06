import { useEffect, useRef, useState } from 'react';
import { actions, useEngine } from '../store';

const SAMPLE_RATES = [44100, 48000, 88200, 96000];
const BUFFER_SIZES = [16, 32, 48, 64, 96, 128, 192, 256, 512, 1024];

export function AudioSettings({ open, onClose }: { open: boolean; onClose: () => void }) {
  const ref = useRef<HTMLDialogElement>(null);
  const devices = useEngine((s) => s.devices);
  const audio = useEngine((s) => s.audio);
  const [type, setType] = useState('');
  const [name, setName] = useState('');
  const [sr, setSr] = useState(48000);
  const [bs, setBs] = useState(128);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    const d = ref.current;
    if (!d) return;
    if (open && !d.open) {
      d.showModal();
      void actions().listDevices();
    } else if (!open && d.open) d.close();
  }, [open]);

  // seed the form from the current device each time the dialog opens
  useEffect(() => {
    if (!open || !audio) return;
    setType(audio.type);
    setName(audio.name);
    setSr(audio.sampleRate);
    setBs(audio.bufferSize);
  }, [open, audio]);

  const names = devices?.available.find((a) => a.type === type)?.names ?? [];
  const types = devices?.types ?? (audio ? [audio.type] : []);
  const latencyMs = (bs / sr) * 1000;
  const changed = !!audio && (audio.type !== type || audio.name !== name || audio.sampleRate !== sr || audio.bufferSize !== bs);

  const apply = async () => {
    setBusy(true);
    await actions().setAudioDevice(type, name, sr, bs);
    setBusy(false);
  };

  return (
    <dialog ref={ref} className="dialog" onClose={onClose} onCancel={onClose}>
      <header className="dialog-head">
        <h2>Audio &amp; MIDI</h2>
        <button type="button" className="icon-btn" onClick={onClose} aria-label="Close">
          ×
        </button>
      </header>
      <div className="dialog-body">
        <section>
          <div className="panel-label">Audio output</div>
          <label className="field">
            <span>Device type</span>
            <select
              className="select"
              value={type}
              onChange={(e) => {
                setType(e.target.value);
                setName(devices?.available.find((a) => a.type === e.target.value)?.names[0] ?? '');
              }}
            >
              {types.map((t) => (
                <option key={t}>{t}</option>
              ))}
            </select>
          </label>
          <label className="field">
            <span>Device</span>
            <select className="select" value={name} onChange={(e) => setName(e.target.value)}>
              {names.map((n) => (
                <option key={n}>{n}</option>
              ))}
              {!names.includes(name) && name && <option>{name}</option>}
            </select>
          </label>
          <div className="field-row">
            <label className="field">
              <span>Sample rate</span>
              <select className="select" value={sr} onChange={(e) => setSr(Number(e.target.value))}>
                {SAMPLE_RATES.map((r) => (
                  <option key={r} value={r}>
                    {r.toLocaleString()} Hz
                  </option>
                ))}
              </select>
            </label>
            <label className="field">
              <span>Buffer size</span>
              <select className="select" value={bs} onChange={(e) => setBs(Number(e.target.value))}>
                {BUFFER_SIZES.map((b) => (
                  <option key={b} value={b}>
                    {b} samples
                  </option>
                ))}
              </select>
            </label>
          </div>
          <div className="latency-calc">
            Buffer latency <b className="mono">{latencyMs.toFixed(2)} ms</b>
            {audio && (
              <span className="muted">
                {' '}
                · current in/out {audio.inputLatencyMs.toFixed(1)} / {audio.outputLatencyMs.toFixed(1)} ms
                {audio.running ? '' : ' · stopped'}
              </span>
            )}
          </div>
          <div className="row gap">
            <button type="button" className="btn primary" disabled={!changed || busy || !name} onClick={() => void apply()}>
              {busy ? 'Applying…' : 'Apply'}
            </button>
            <button type="button" className="btn" onClick={() => void actions().listDevices()}>
              Rescan devices
            </button>
          </div>
        </section>
        <section>
          <div className="panel-label">MIDI inputs</div>
          <ul className="midi-list">
            {(devices?.midiInputs ?? []).map((m) => (
              <li key={m}>
                <span className="led on" /> {m}
              </li>
            ))}
            {devices && devices.midiInputs.length === 0 && <li className="muted">No MIDI inputs found.</li>}
          </ul>
          <button type="button" className="btn" onClick={() => void actions().rescanMidi()}>
            Rescan MIDI
          </button>
          <p className="hint">All inputs are opened automatically (omni); layers filter by channel.</p>
        </section>
      </div>
    </dialog>
  );
}
