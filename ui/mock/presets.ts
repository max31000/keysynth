// Fake factory presets for the mock engine (PRESETS.md format, without node ids — the mock assigns them).

import type { FxSlot, Layer, Patch, Zone } from '../src/protocol/types';
import { defaultParams } from './catalog';

const zone = (z: Partial<Zone> = {}): Zone => ({
  key_lo: 21,
  key_hi: 108,
  vel_lo: 1,
  vel_hi: 127,
  transpose: 0,
  channel: 0,
  volume_db: 0,
  pan: 0,
  mute: false,
  solo: false,
  sustain: true,
  ...z,
});

const fx = (type: string, params: Record<string, number> = {}, bypass = false): FxSlot => ({
  type,
  bypass,
  params: { ...defaultParams(type), ...params },
});

const layer = (name: string, type: string, params: Record<string, number>, fxs: FxSlot[] = [], z: Partial<Zone> = {}): Layer => ({
  name,
  zone: zone(z),
  instrument: { type, params: { ...defaultParams(type), ...params }, state: {} },
  fx: fxs,
});

interface Def {
  slug: string;
  name: string;
  category: string;
  tags: string[];
  description?: string;
  tempo?: number;
  layers: Layer[];
  master?: FxSlot[];
}

const DEFS: Def[] = [
  {
    slug: 'organ/light-my-fire',
    name: 'Light My Fire Organ',
    category: 'Organ',
    tags: ['doors', '60s', 'combo'],
    description: 'Vox Continental-style combo organ. Reference: The Doors — Light My Fire.',
    layers: [layer('Organ', 'combo', { foot_8: 1, foot_4: 0.7 }, [fx('drive', { amount: 0.2 }), fx('reverb', { mix: 0.18, size: 0.6 })], { key_lo: 36 })],
  },
  {
    slug: 'organ/echoes-b3',
    name: 'Echoes B3',
    category: 'Organ',
    tags: ['floyd', '70s', 'tonewheel'],
    description: 'Tonewheel through a Leslie. Reference: Pink Floyd — Echoes.',
    layers: [layer('B3', 'organ', { drawbar_1: 8, drawbar_2: 8, drawbar_3: 8, drawbar_4: 4 }, [fx('rotary'), fx('reverb', { mix: 0.25, type: 0 })])],
  },
  {
    slug: 'organ/gospel-full',
    name: 'Gospel Full Drawbars',
    category: 'Organ',
    tags: ['tonewheel', 'gospel'],
    layers: [layer('B3', 'organ', { drawbar_1: 8, drawbar_2: 8, drawbar_3: 8, drawbar_4: 8, drawbar_5: 6, drawbar_6: 8, drawbar_9: 8 }, [fx('rotary', { speed: 1 })])],
  },
  {
    slug: 'synth-lead/take-on-me',
    name: 'Take On Me Lead',
    category: 'Synth Lead',
    tags: ['80s', 'a-ha', 'juno'],
    description: 'Bright poly lead. Reference: a-ha — Take On Me.',
    tempo: 169,
    layers: [layer('Lead', 'va', { osc1_wave: 0, osc2_wave: 1, cutoff: 5200, resonance: 0.15, amp_release: 180 }, [fx('chorus', { mode: 1 }), fx('delay', { time: 355, mix: 0.18 })])],
  },
  {
    slug: 'synth-lead/floyd-minimoog',
    name: 'Shine On Lead',
    category: 'Synth Lead',
    tags: ['floyd', '70s', 'mono'],
    layers: [layer('Lead', 'va', { voice_mode: 2, glide: 80, cutoff: 1800, resonance: 0.35 }, [fx('delay', { mode: 2, mix: 0.3 }), fx('reverb', { mix: 0.3, decay: 4 })])],
  },
  {
    slug: 'synth-pad/jump-brass',
    name: 'Jump Brass',
    category: 'Brass',
    tags: ['80s', 'van halen', 'oberheim'],
    description: 'OB-Xa brass stab. Reference: Van Halen — Jump.',
    tempo: 130,
    layers: [layer('Brass', 'va', { osc1_wave: 0, osc2_wave: 0, osc2_detune: 9, fenv_attack: 30, env_amount: 0.6 }, [fx('chorus', { mode: 0, mix: 0.3 }), fx('reverb', { type: 1, mix: 0.15 })])],
  },
  {
    slug: 'synth-pad/warm-strings',
    name: 'Warm Analog Strings',
    category: 'Synth Pad',
    tags: ['pad', 'juno', 'ensemble'],
    layers: [layer('Pad', 'va', { amp_attack: 600, amp_release: 1800, cutoff: 1400, unison: 3 }, [fx('chorus', { mode: 2 }), fx('reverb', { mix: 0.35, decay: 5 })])],
  },
  {
    slug: 'synth-pad/rammstein-supersaw',
    name: 'Sonne Supersaw',
    category: 'Synth Pad',
    tags: ['rammstein', 'industrial', 'supersaw'],
    layers: [layer('Saw', 'va', { osc1_wave: 4, unison: 7, unison_detune: 0.45, cutoff: 6000 }, [fx('eq', { low_gain: -3 }), fx('compressor'), fx('reverb', { mix: 0.22 })])],
  },
  {
    slug: 'synth-bass/moog-bass',
    name: 'Round Mono Bass',
    category: 'Synth Bass',
    tags: ['bass', 'mono'],
    layers: [layer('Bass', 'va', { voice_mode: 1, osc1_octave: -1, mix_sub: 0.7, cutoff: 600, resonance: 0.3 }, [fx('compressor', { ratio: 6 })], { key_hi: 59 })],
  },
  {
    slug: 'e-piano/riders-rhodes',
    name: 'Riders Rhodes',
    category: 'E.Piano',
    tags: ['doors', 'rhodes', '70s'],
    description: 'Rhodes with tremolo. Reference: The Doors — Riders on the Storm.',
    layers: [layer('Rhodes', 'epiano', { model: 0, tremolo_depth: 0.5 }, [fx('phaser', { mix: 0.25 }), fx('reverb', { mix: 0.2 })])],
  },
  {
    slug: 'e-piano/wurli-money',
    name: 'Money Wurli',
    category: 'E.Piano',
    tags: ['floyd', 'wurlitzer'],
    layers: [layer('Wurli', 'epiano', { model: 2, bark: 0.5 }, [fx('drive', { amount: 0.35 })])],
  },
  {
    slug: 'splits/bass-and-rhodes',
    name: 'Bass + Rhodes Split',
    category: 'Splits & Layers',
    tags: ['split', 'rhodes', 'bass'],
    layers: [
      layer('Piano Bass', 'epiano', { model: 3 }, [], { key_lo: 21, key_hi: 52 }),
      layer('Rhodes', 'epiano', { model: 0 }, [fx('chorus', { mix: 0.3 })], { key_lo: 53, key_hi: 108 }),
    ],
    master: [fx('reverb', { mix: 0.15 })],
  },
  {
    slug: 'splits/organ-over-pad',
    name: 'Organ over Pad',
    category: 'Splits & Layers',
    tags: ['layer', 'organ', 'pad'],
    layers: [
      layer('B3', 'organ', {}, [fx('rotary')]),
      layer('Pad', 'va', { amp_attack: 800, amp_release: 2000 }, [fx('reverb', { mix: 0.4 })], { volume_db: -8 }),
    ],
  },
];

export interface MockPreset {
  path: string;
  name: string;
  category: string;
  tags: string[];
  factory: boolean;
  patch: Patch;
}

export function factoryPresets(): MockPreset[] {
  return DEFS.map((d) => ({
    path: `presets/factory/${d.slug}.json`,
    name: d.name,
    category: d.category,
    tags: d.tags,
    factory: true,
    patch: {
      format: 2,
      meta: { name: d.name, category: d.category, tags: d.tags, description: d.description ?? '', author: 'factory' },
      tempo: d.tempo ?? 120,
      layers: d.layers,
      master: { volume_db: -3, fx: d.master ?? [] },
    },
  }));
}

export const INIT_PATCH = (): Patch => ({
  format: 2,
  meta: { name: 'Init', category: 'Synth Lead', tags: [] },
  tempo: 120,
  layers: [layer('Layer 1', 'va', {})],
  master: { volume_db: -3, fx: [] },
});

export { layer as makeLayer, fx as makeFx };
