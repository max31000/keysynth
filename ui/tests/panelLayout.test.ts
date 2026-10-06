import { describe, expect, it } from 'vitest';
import { AUTO_TAB_THRESHOLD, FRONT_TAB, groupParams, panelTabs } from '../src/lib/panelLayout';
import { LATENCY_WARN_MS, bufferMs, latencyTooHigh } from '../src/lib/latency';
import type { AudioStatus, ModuleInfo, ParamSpec } from '../src/protocol/types';

const p = (id: string, group: string, flags = 0): ParamSpec => ({
  id,
  name: id,
  group,
  unit: '',
  min: 0,
  max: 1,
  def: 0,
  scale: 'linear',
  skewCentre: 0,
  flags,
  choices: [],
});

const mod = (params: ParamSpec[], uiHints?: ModuleInfo['uiHints']): ModuleInfo => ({
  typeId: 't',
  name: 'T',
  kind: 'instrument',
  category: '',
  params,
  ...(uiHints ? { uiHints } : {}),
});

describe('panelTabs', () => {
  it('keeps small modules flat', () => {
    expect(panelTabs(mod([p('a', 'A'), p('b', 'B')], { front: ['a'] }))).toBeNull();
  });

  it('bundles groups by uiHints.tabs, unlisted groups get their own tab, Front first', () => {
    const params = ['Main', 'Op 1', 'Op 2', 'LFO'].flatMap((g) => [p(`${g}_x`, g), p(`${g}_y`, g)]);
    const tabs = panelTabs(
      mod(params, {
        front: ['Main_x', 'Op 1_x', 'missing'],
        tabs: [
          { name: 'Voice', groups: ['Main', 'Nope'] },
          { name: 'Op 1', groups: ['Op 1'] },
        ],
      }),
    )!;
    expect(tabs.map((t) => t.name)).toEqual([FRONT_TAB, 'Voice', 'Op 1', 'Op 2', 'LFO']);
    expect(tabs[0]!.groups[0]!.params.map((x) => x.id)).toEqual(['Main_x', 'Op 1_x']);
    expect(tabs[1]!.groups.map((g) => g.name)).toEqual(['Main']);
  });

  it('auto-tabs large modules per group and skips hidden params', () => {
    const params = Array.from({ length: AUTO_TAB_THRESHOLD + 1 }, (_, i) => p(`p${i}`, i % 2 ? 'Odd' : 'Even'));
    params.push(p('secret', 'Hidden', 2));
    const tabs = panelTabs(mod(params))!;
    expect(tabs.map((t) => t.name)).toEqual(['Even', 'Odd']);
    expect(groupParams(mod(params)).some((g) => g.name === 'Hidden')).toBe(false);
  });
});

describe('latency', () => {
  const a = (outputLatencyMs: number, running = true): AudioStatus => ({
    type: 'ASIO',
    name: 'Yamaha Steinberg USB ASIO',
    sampleRate: 48000,
    bufferSize: 1024,
    inputLatencyMs: 25.8,
    outputLatencyMs,
    running,
  });
  it('warns above the threshold only for a running device', () => {
    expect(latencyTooHigh(a(29.8))).toBe(true);
    expect(latencyTooHigh(a(LATENCY_WARN_MS))).toBe(false);
    expect(latencyTooHigh(a(29.8, false))).toBe(false);
    expect(latencyTooHigh(null)).toBe(false);
    expect(bufferMs(a(0))).toBeCloseTo(21.33, 2);
  });
});
