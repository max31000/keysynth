// Moog-style 24 dB ladder low-pass (stdfaust ve.moog_vcf) with drive and an envelope-free LFO.
// Faust JIT effect example - docs/PLUGINS.md.
declare name "Moog VCF";
declare author "keysynth";
declare license "AGPL-3.0-or-later";

import("stdfaust.lib");

cutoff    = hslider("h:Filter/[0]cutoff [unit:Hz][scale:log]", 1200, 30, 16000, 1);
resonance = hslider("h:Filter/[1]resonance", 0.5, 0, 0.97, 0.01) : si.smoo;
drive     = hslider("h:Filter/[2]drive [unit:dB]", 0, 0, 24, 0.1) : ba.db2linear : si.smoo;
lfoRate   = hslider("h:LFO/[0]rate [unit:Hz][scale:log]", 0.5, 0.05, 10, 0.01);
lfoDepth  = hslider("h:LFO/[1]depth [unit:oct]", 0, 0, 4, 0.01) : si.smoo;
mix       = hslider("h:Output/[0]mix", 1, 0, 1, 0.01) : si.smoo;
level     = hslider("h:Output/[1]level [unit:dB]", 0, -24, 12, 0.1) : ba.db2linear : si.smoo;

// Cutoff modulated in octaves by a sine LFO, smoothed, kept below Nyquist.
fc = cutoff * pow(2, lfoDepth * os.osc(lfoRate)) : min(0.45 * ma.SR) : max(20) : si.smoo;

// Soft-clipped drive into the ladder; loudness roughly compensated for drive.
ladder(x) = x * drive : ma.tanh : ve.moog_vcf(resonance, fc) : /(sqrt(drive));
voice(x) = (x * (1 - mix) + ladder(x) * mix) * level;

process = voice, voice;
