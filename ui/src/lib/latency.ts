import type { AudioStatus } from '../protocol/types';

/** Above this device-reported output latency playing feels sluggish: warn and point at the driver panel. */
export const LATENCY_WARN_MS = 8;

export function bufferMs(a: Pick<AudioStatus, 'bufferSize' | 'sampleRate'>): number {
  return a.sampleRate > 0 ? (a.bufferSize / a.sampleRate) * 1000 : 0;
}

export function latencyTooHigh(a: AudioStatus | null | undefined): boolean {
  return !!a && a.running !== false && a.outputLatencyMs > LATENCY_WARN_MS;
}

/** What to do about high latency: drivers with their own panel (ASIO) only take the size set there. */
export function latencyHint(a: Pick<AudioStatus, 'hasControlPanel'> | null | undefined): string {
  return a?.hasControlPanel
    ? 'pick 64 or 128 samples in the driver panel'
    : 'pick a buffer of 64 or 128 samples in the audio settings';
}
