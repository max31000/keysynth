// Fake module catalog for the mock engine. Shapes follow docs/PROTOCOL.md "Wire details".

import type { ModuleInfo, ParamScale, ParamSpec } from '../src/protocol/types';

interface Opt {
  min?: number;
  max?: number;
  def?: number;
  scale?: ParamScale;
  unit?: string;
  skew?: number;
  flags?: number;
  choices?: string[];
}

function p(id: string, name: string, group: string, o: Opt = {}): ParamSpec {
  const choices = o.choices ?? [];
  const scale: ParamScale = o.scale ?? (choices.length ? 'enum' : 'linear');
  return {
    id,
    name,
    group,
    unit: o.unit ?? '',
    min: o.min ?? 0,
    max: o.max ?? (scale === 'enum' ? choices.length - 1 : 1),
    def: o.def ?? 0,
    scale,
    skewCentre: o.skew ?? 0,
    flags: o.flags ?? 0,
    choices,
  };
}

const bool = (id: string, name: string, group: string, def = 0) => p(id, name, group, { scale: 'bool', def });
const ms = (id: string, name: string, group: string, def: number, max = 10000, skew = 500) =>
  p(id, name, group, { min: 1, max, def, scale: 'log', unit: 'ms', skew });
const WAVES = ['Saw', 'Pulse', 'Triangle', 'Sine', 'Supersaw', 'Noise'];

const va: ModuleInfo = {
  typeId: 'va',
  name: 'Virtual Analog',
  kind: 'instrument',
  category: 'Synth',
  uiHints: {
    groupOrder: ['Osc 1', 'Osc 2', 'Mixer', 'Filter', 'Filter Env', 'Amp Env', 'LFO', 'Voice'],
    front: ['cutoff', 'resonance', 'env_amount', 'amp_release'],
  },
  params: [
    p('osc1_wave', 'Wave', 'Osc 1', { choices: WAVES }),
    p('osc1_octave', 'Octave', 'Osc 1', { min: -2, max: 2, scale: 'int' }),
    p('osc1_pw', 'Pulse Width', 'Osc 1', { min: 0.05, max: 0.95, def: 0.5 }),
    p('osc2_wave', 'Wave', 'Osc 2', { choices: WAVES, def: 1 }),
    p('osc2_semi', 'Semitone', 'Osc 2', { min: -24, max: 24, scale: 'int', unit: 'st' }),
    p('osc2_detune', 'Detune', 'Osc 2', { min: -50, max: 50, def: 7, unit: 'ct' }),
    bool('osc2_sync', 'Sync', 'Osc 2'),
    p('mix_osc1', 'Osc 1', 'Mixer', { def: 0.8 }),
    p('mix_osc2', 'Osc 2', 'Mixer', { def: 0.6 }),
    p('mix_sub', 'Sub', 'Mixer', { def: 0 }),
    p('mix_noise', 'Noise', 'Mixer', { def: 0 }),
    p('filter_type', 'Type', 'Filter', { choices: ['Ladder 24', 'SVF LP', 'SVF BP', 'SVF HP'] }),
    p('cutoff', 'Cutoff', 'Filter', { min: 20, max: 20000, def: 2400, scale: 'log', unit: 'Hz', skew: 1000 }),
    p('resonance', 'Resonance', 'Filter', { def: 0.2 }),
    p('env_amount', 'Env Amount', 'Filter', { min: -1, max: 1, def: 0.4 }),
    p('key_track', 'Key Track', 'Filter', { def: 0.5 }),
    ms('fenv_attack', 'Attack', 'Filter Env', 5),
    ms('fenv_decay', 'Decay', 'Filter Env', 400),
    p('fenv_sustain', 'Sustain', 'Filter Env', { def: 0.3 }),
    ms('fenv_release', 'Release', 'Filter Env', 300),
    ms('amp_attack', 'Attack', 'Amp Env', 2),
    ms('amp_decay', 'Decay', 'Amp Env', 600),
    p('amp_sustain', 'Sustain', 'Amp Env', { def: 0.8 }),
    ms('amp_release', 'Release', 'Amp Env', 350),
    p('lfo1_rate', 'Rate', 'LFO', { min: 0.01, max: 40, def: 4, scale: 'log', unit: 'Hz', skew: 2 }),
    p('lfo1_shape', 'Shape', 'LFO', { choices: ['Sine', 'Tri', 'Square', 'S&H'] }),
    p('lfo1_to_pitch', 'Vibrato', 'LFO', { def: 0 }),
    p('lfo1_to_cutoff', 'To Cutoff', 'LFO', { def: 0 }),
    p('voice_mode', 'Mode', 'Voice', { choices: ['Poly', 'Mono', 'Legato'] }),
    p('unison', 'Unison', 'Voice', { min: 1, max: 8, def: 1, scale: 'int' }),
    p('unison_detune', 'Uni Detune', 'Voice', { def: 0.2 }),
    ms('glide', 'Glide', 'Voice', 1, 2000, 100),
    p('volume', 'Volume', 'Voice', { min: -48, max: 6, def: -6, unit: 'dB' }),
    p('active_voices', 'Voices', 'Voice', { min: 0, max: 16, scale: 'int', flags: 1 }),
  ],
};

const DRAWBARS = ["16'", "5⅓'", "8'", "4'", "2⅔'", "2'", "1⅗'", "1⅓'", "1'"];
const organ: ModuleInfo = {
  typeId: 'organ',
  name: 'Tonewheel Organ',
  kind: 'instrument',
  category: 'Organ',
  uiHints: {
    groupOrder: ['Drawbars', 'Percussion', 'Vibrato', 'Character'],
    controls: Object.fromEntries(DRAWBARS.map((_, i) => [`drawbar_${i + 1}`, 'drawbar' as const])),
  },
  params: [
    ...DRAWBARS.map((n, i) =>
      p(`drawbar_${i + 1}`, n, 'Drawbars', { min: 0, max: 8, scale: 'int', def: [8, 8, 8, 0, 0, 0, 0, 0, 0][i] }),
    ),
    bool('perc', 'Percussion', 'Percussion', 1),
    p('perc_harmonic', 'Harmonic', 'Percussion', { choices: ['2nd', '3rd'], def: 1 }),
    p('perc_decay', 'Decay', 'Percussion', { choices: ['Fast', 'Slow'] }),
    p('perc_volume', 'Volume', 'Percussion', { choices: ['Normal', 'Soft'] }),
    p('vibrato', 'Mode', 'Vibrato', { choices: ['Off', 'V1', 'V2', 'V3', 'C1', 'C2', 'C3'], def: 6 }),
    p('click', 'Key Click', 'Character', { def: 0.4 }),
    p('crosstalk', 'Crosstalk', 'Character', { def: 0.15 }),
    p('drive', 'Preamp Drive', 'Character', { def: 0.2 }),
  ],
};

const epiano: ModuleInfo = {
  typeId: 'epiano',
  name: 'Electric Piano',
  kind: 'instrument',
  category: 'E.Piano',
  params: [
    p('model', 'Model', 'Model', { choices: ['Rhodes Mk I', 'Rhodes Mk II', 'Wurlitzer 200A', 'Piano Bass'] }),
    p('hardness', 'Hammer', 'Tone', { def: 0.5 }),
    p('bark', 'Bark', 'Tone', { def: 0.3 }),
    p('pickup', 'Pickup Pos', 'Tone', { def: 0.5 }),
    p('tremolo_rate', 'Rate', 'Tremolo', { min: 0.5, max: 12, def: 4.5, scale: 'log', unit: 'Hz' }),
    p('tremolo_depth', 'Depth', 'Tremolo', { def: 0 }),
    p('volume', 'Volume', 'Output', { min: -48, max: 6, def: -6, unit: 'dB' }),
  ],
};

const combo: ModuleInfo = {
  typeId: 'combo',
  name: 'Combo Organ',
  kind: 'instrument',
  category: 'Organ',
  params: [
    p('voicing', 'Voicing', 'Voicing', { choices: ['Vox Continental', 'Farfisa Compact'] }),
    p('foot_16', "16'", 'Footages', { def: 0 }),
    p('foot_8', "8'", 'Footages', { def: 1 }),
    p('foot_4', "4'", 'Footages', { def: 0.6 }),
    p('foot_ii', 'IV', 'Footages', { def: 0 }),
    bool('vibrato', 'Vibrato', 'Vibrato', 1),
    p('vibrato_rate', 'Rate', 'Vibrato', { min: 2, max: 9, def: 5.6, unit: 'Hz' }),
    p('volume', 'Volume', 'Output', { min: -48, max: 6, def: -6, unit: 'dB' }),
  ],
};

const fx = (typeId: string, name: string, category: string, params: ParamSpec[]): ModuleInfo => ({
  typeId,
  name,
  kind: 'effect',
  category,
  params,
});

const effects: ModuleInfo[] = [
  fx('chorus', 'Chorus', 'Modulation', [
    p('mode', 'Mode', 'Main', { choices: ['I', 'II', 'I+II'] }),
    p('mix', 'Mix', 'Main', { def: 0.5 }),
  ]),
  fx('phaser', 'Phaser', 'Modulation', [
    p('rate', 'Rate', 'Main', { min: 0.02, max: 10, def: 0.4, scale: 'log', unit: 'Hz' }),
    p('depth', 'Depth', 'Main', { def: 0.7 }),
    p('feedback', 'Feedback', 'Main', { def: 0.3 }),
    p('stages', 'Stages', 'Main', { choices: ['4', '6', '8', '12'] }),
    p('mix', 'Mix', 'Main', { def: 0.5 }),
  ]),
  fx('rotary', 'Rotary', 'Modulation', [
    p('speed', 'Speed', 'Main', { choices: ['Slow', 'Fast'] }),
    ms('ramp', 'Ramp', 'Main', 900, 5000, 900),
    p('horn_mix', 'Horn/Drum', 'Main', { def: 0.5 }),
    p('drive', 'Drive', 'Main', { def: 0.2 }),
    p('horn_rpm', 'Horn RPM', 'Meters', { min: 0, max: 420, unit: 'rpm', flags: 1 }),
  ]),
  fx('delay', 'Delay', 'Time', [
    p('mode', 'Mode', 'Main', { choices: ['Stereo', 'Ping-Pong', 'Tape'] }),
    bool('sync', 'Tempo Sync', 'Main'),
    ms('time', 'Time', 'Main', 375, 2000, 300),
    p('feedback', 'Feedback', 'Main', { def: 0.35, max: 0.98 }),
    p('tone', 'Tone', 'Main', { min: 500, max: 16000, def: 6000, scale: 'log', unit: 'Hz' }),
    p('mix', 'Mix', 'Main', { def: 0.25 }),
  ]),
  fx('reverb', 'Reverb', 'Time', [
    p('type', 'Type', 'Main', { choices: ['Hall', 'Plate', 'Room', 'Gated'] }),
    p('size', 'Size', 'Main', { def: 0.6 }),
    p('decay', 'Decay', 'Main', { min: 0.1, max: 20, def: 2.4, scale: 'log', unit: 's', skew: 2 }),
    ms('predelay', 'Pre-delay', 'Main', 10, 250, 30),
    p('damping', 'Damping', 'Main', { def: 0.4 }),
    p('mix', 'Mix', 'Main', { def: 0.2 }),
  ]),
  fx('drive', 'Drive', 'Distortion', [
    p('amount', 'Amount', 'Main', { def: 0.3 }),
    p('tone', 'Tone', 'Main', { def: 0.5 }),
    p('character', 'Character', 'Main', { choices: ['Tube', 'Tape', 'Fuzz'] }),
    p('output', 'Output', 'Main', { min: -24, max: 12, def: 0, unit: 'dB' }),
  ]),
  fx('compressor', 'Compressor', 'Dynamics', [
    p('threshold', 'Threshold', 'Main', { min: -60, max: 0, def: -18, unit: 'dB' }),
    p('ratio', 'Ratio', 'Main', { min: 1, max: 20, def: 4, scale: 'log', unit: ':1', skew: 4 }),
    p('attack', 'Attack', 'Main', { min: 0.1, max: 200, def: 10, scale: 'log', unit: 'ms', skew: 10 }),
    p('release', 'Release', 'Main', { min: 5, max: 2000, def: 150, scale: 'log', unit: 'ms', skew: 150 }),
    p('makeup', 'Makeup', 'Main', { min: 0, max: 24, def: 4, unit: 'dB' }),
    p('gr', 'Gain Reduction', 'Meters', { min: -30, max: 0, unit: 'dB', flags: 1 }),
  ]),
  fx('eq', 'EQ', 'Filter', [
    p('low_gain', 'Low', 'Bands', { min: -15, max: 15, unit: 'dB' }),
    p('low_freq', 'Low Freq', 'Bands', { min: 30, max: 500, def: 120, scale: 'log', unit: 'Hz' }),
    p('mid_gain', 'Mid', 'Bands', { min: -15, max: 15, unit: 'dB' }),
    p('mid_freq', 'Mid Freq', 'Bands', { min: 200, max: 8000, def: 1200, scale: 'log', unit: 'Hz' }),
    p('high_gain', 'High', 'Bands', { min: -15, max: 15, unit: 'dB' }),
    p('high_freq', 'High Freq', 'Bands', { min: 2000, max: 18000, def: 8000, scale: 'log', unit: 'Hz' }),
  ]),
];

export const CATALOG: ModuleInfo[] = [va, organ, combo, epiano, ...effects];
export const catalogMap: Record<string, ModuleInfo> = Object.fromEntries(CATALOG.map((m) => [m.typeId, m]));

export function defaultParams(typeId: string): Record<string, number> {
  const m = catalogMap[typeId];
  if (!m) return {};
  return Object.fromEntries(m.params.filter((s) => !(s.flags & 1)).map((s) => [s.id, s.def]));
}
