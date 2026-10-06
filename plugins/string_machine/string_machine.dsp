// String machine (Solina / ARP String Ensemble style): divide-down style saw registers, per-voice
// three-phase "ensemble" pitch modulation, slow attack/release, gentle low-pass. Stereo out.
// Faust JIT instrument example - docs/PLUGINS.md.
declare name "String Machine";
declare author "keysynth";
declare license "AGPL-3.0-or-later";

import("stdfaust.lib");

freq = hslider("freq", 440, 20, 8000, 0.01);
gain = hslider("gain", 0.8, 0, 1, 0.01);
gate = button("gate");

viola   = hslider("h:Registers/[0]viola_8", 0.8, 0, 1, 0.01) : si.smoo;  // 8'
violin  = hslider("h:Registers/[1]violin_4", 0.5, 0, 1, 0.01) : si.smoo; // 4'
cello   = hslider("h:Registers/[2]cello_16", 0.3, 0, 1, 0.01) : si.smoo; // 16'
attack  = hslider("h:Envelope/[0]attack [unit:s][scale:log]", 0.25, 0.005, 4, 0.001);
release = hslider("h:Envelope/[1]release [unit:s][scale:log]", 0.8, 0.02, 6, 0.001);
ens     = hslider("h:Ensemble/[0]depth", 0.6, 0, 1, 0.01) : si.smoo;
bright  = hslider("h:Tone/[0]brightness [unit:Hz][scale:log]", 5000, 500, 16000, 1) : si.smoo;
level   = hslider("h:Tone/[1]level [unit:dB]", -12, -40, 6, 0.1) : ba.db2linear : si.smoo;

// Three-phase ensemble: slow (0.63 Hz) + fast (6.3 Hz) vibrato, phases 0/120/240 degrees.
vib(ph) = 1 + ens * (0.0035 * os.oscp(0.63, ph) + 0.0012 * os.oscp(6.3, ph * 1.7));
reg(f, ph) = os.sawtooth(f * vib(ph));
section(f, p0) = (reg(f, p0) + reg(f, p0 + 2.094) + reg(f, p0 + 4.189)) / 3;
tone(p0) = viola * section(freq, p0) + violin * section(2 * freq, p0 + 0.5) + cello * section(0.5 * freq, p0 + 1.0);

env = en.asr(attack, 1, release, gate);
amp = env * (0.5 + 0.5 * gain) * level;
voice(p0) = tone(p0) : fi.lowpass(2, bright) : *(amp);

process = voice(0), voice(1.047); // slightly different ensemble phases per side = width
