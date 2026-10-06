// Simple stereo tremolo (amplitude LFO). Faust JIT effect example - docs/PLUGINS.md.
declare name "Simple Tremolo";
declare author "keysynth";
declare license "AGPL-3.0-or-later";

import("stdfaust.lib");

rate  = hslider("rate [unit:Hz][scale:log]", 5, 0.1, 20, 0.01);
depth = hslider("depth", 0.5, 0, 1, 0.01) : si.smoo;
shape = nentry("shape [style:menu{'Sine':0;'Triangle':1}]", 0, 0, 1, 1);
// Stereo spread: phase offset of the right channel's LFO (0 = mono tremolo, 0.5 = auto-pan).
spread = hslider("spread", 0, 0, 0.5, 0.01) : si.smoo;

// Unipolar LFO 0..1 with a phase offset (in cycles).
lfo(ph) = select2(shape, sine, tri)
with {
    p    = os.phasor(1, rate) + ph : ma.frac;
    sine = 0.5 + 0.5 * sin(2 * ma.PI * p);
    tri  = 1 - abs(2 * p - 1);
};

gainAt(ph) = 1 - depth * lfo(ph);

// The left LFO is also shown as a read-only meter (bargraph -> ReadOnly param `lfo`).
process = _, _ : *(gainAt(0) : attach(_, lfo(0) : hbargraph("lfo", 0, 1))), *(gainAt(spread));
