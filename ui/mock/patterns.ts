// Pattern files for the mock engine: reads the real presets/patterns/*.json (string or array steps, PRESETS.md)
// and converts them to the canonical wire form the engine sends (number arrays).

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import type { Pattern, PatternEntry } from '../src/protocol/types';
import { emptyPattern, normalizePattern } from '../src/lib/stepGrid';

const KITS: Record<string, number> = { '808': 0, '909': 1, linn: 2, industrial: 3 };

function charVelocity(c: string): number | null {
  if (c === ' ' || c === '|' || c === '\t') return null;
  if (c === 'x') return 100;
  if (c === 'X') return 127;
  if (c === 'o' || c === 'O') return 50;
  if (c >= '1' && c <= '9') return Math.round((Number(c) * 127) / 9);
  return 0;
}

function parseRow(v: unknown, binary: boolean): number[] {
  let out: number[] = [];
  if (typeof v === 'string') {
    for (const c of v) {
      const x = charVelocity(c);
      if (x !== null) out.push(x);
    }
  } else if (Array.isArray(v)) {
    out = v.map((x) => (typeof x === 'number' && Number.isFinite(x) ? Math.round(x) : x === true ? 100 : 0));
  }
  return binary ? out.map((x) => (x > 0 ? 1 : 0)) : out;
}

/** Lenient file → canonical Pattern (same rules as engine patternFromJson). */
export function parsePatternJson(j: Record<string, unknown>): Pattern {
  const raw = Array.isArray(j.time_sig) ? (j.time_sig as unknown[]) : [];
  const ts: [number, number] = [typeof raw[0] === 'number' ? raw[0] : 4, typeof raw[1] === 'number' ? raw[1] : 4];
  const tracks = Array.isArray(j.tracks) ? (j.tracks as Record<string, unknown>[]) : [];
  const kit = typeof j.kit === 'string' ? KITS[j.kit.toLowerCase()] : typeof j.kit === 'number' ? j.kit : undefined;
  const parsed = tracks.map((t) => ({
    name: typeof t.name === 'string' ? t.name : `Note ${String(t.note)}`,
    note: typeof t.note === 'number' ? t.note : 36,
    mute: t.mute === true,
    steps: parseRow(t.steps, false),
  }));
  const spb = typeof j.steps_per_beat === 'number' ? j.steps_per_beat : 4;
  let bars = typeof j.bars === 'number' ? j.bars : 0;
  if (!bars) {
    const longest = Math.max(0, ...parsed.map((t) => t.steps.length));
    bars = Math.max(1, Math.ceil(longest / (ts[0] * spb)));
  }
  const p: Pattern = {
    format: 1,
    name: typeof j.name === 'string' ? j.name : 'Untitled',
    description: typeof j.description === 'string' ? j.description : '',
    time_sig: [ts[0], ts[1]],
    steps_per_beat: spb,
    bars,
    swing: typeof j.swing === 'number' ? j.swing : 0,
    accent_amount: typeof j.accent_amount === 'number' ? j.accent_amount : 0.5,
    tracks: parsed,
    accent: parseRow(j.accent, true),
  };
  if (typeof j.tempo === 'number') p.tempo = j.tempo;
  if (kit !== undefined) p.kit = kit;
  return normalizePattern(p);
}

export interface MockPattern {
  entry: PatternEntry;
  pattern: Pattern;
}

/** Factory patterns from <repo>/presets/patterns (empty list if the directory is missing). */
export function factoryPatterns(): MockPattern[] {
  const dir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'presets', 'patterns');
  let files: string[];
  try {
    files = fs.readdirSync(dir).filter((f) => f.endsWith('.json')).sort();
  } catch {
    return [];
  }
  const out: MockPattern[] = [];
  for (const f of files) {
    try {
      const pattern = parsePatternJson(JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8')) as Record<string, unknown>);
      out.push({
        pattern,
        entry: {
          path: `presets/patterns/${f}`,
          name: pattern.name,
          time_sig: pattern.time_sig,
          bars: pattern.bars,
          tempo: pattern.tempo ?? 0,
          factory: true,
        },
      });
    } catch {
      // skip unreadable files
    }
  }
  return out;
}

export { emptyPattern };
