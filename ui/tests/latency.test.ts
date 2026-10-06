import { describe, expect, it } from 'vitest';
import { LATENCY_WARN_MS, bufferMs, latencyHint, latencyTooHigh } from '../src/lib/latency';
import type { AudioStatus } from '../src/protocol/types';

const audio = (o: Partial<AudioStatus> = {}): AudioStatus => ({
  type: 'ASIO',
  name: 'Dev',
  sampleRate: 48000,
  bufferSize: 1024,
  inputLatencyMs: 22,
  outputLatencyMs: 23.5,
  running: true,
  hasControlPanel: true,
  ...o,
});

describe('latency helpers', () => {
  it('bufferMs converts samples to ms and guards a zero rate', () => {
    expect(bufferMs({ bufferSize: 480, sampleRate: 48000 })).toBeCloseTo(10);
    expect(bufferMs({ bufferSize: 64, sampleRate: 0 })).toBe(0);
  });

  it('latencyTooHigh: only running devices above the threshold', () => {
    expect(latencyTooHigh(audio())).toBe(true);
    expect(latencyTooHigh(audio({ outputLatencyMs: LATENCY_WARN_MS }))).toBe(false);
    expect(latencyTooHigh(audio({ running: false }))).toBe(false);
    expect(latencyTooHigh(null)).toBe(false);
    expect(latencyTooHigh(undefined)).toBe(false);
  });

  it('latencyHint points at the driver panel only when there is one', () => {
    expect(latencyHint(audio())).toMatch(/driver panel/);
    expect(latencyHint(audio({ hasControlPanel: false }))).not.toMatch(/driver panel/);
    expect(latencyHint(audio({ hasControlPanel: undefined }))).toMatch(/audio settings/);
    expect(latencyHint(null)).toMatch(/audio settings/);
  });
});
