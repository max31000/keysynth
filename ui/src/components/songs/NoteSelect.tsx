// MIDI note picker shown as note names (A0, C4 = 60); «—» = none (null).

import { noteName } from '../../lib/notes';

const NOTES = Array.from({ length: 128 }, (_, n) => n);

export function NoteSelect({
  value,
  onChange,
  label,
  disabled,
}: {
  value: number | null;
  onChange(v: number | null): void;
  label: string;
  disabled?: boolean;
}) {
  return (
    <select
      className="select note-select mono"
      aria-label={label}
      value={value === null ? '' : String(value)}
      disabled={disabled}
      onChange={(e) => onChange(e.target.value === '' ? null : Number(e.target.value))}
    >
      <option value="">—</option>
      {NOTES.map((n) => (
        <option key={n} value={n}>
          {noteName(n)}
        </option>
      ))}
    </select>
  );
}
