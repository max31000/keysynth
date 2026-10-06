// Karplus-Strong plucked string. Faust JIT instrument example - docs/PLUGINS.md.
// Voice convention: freq (Hz), gain (velocity 0..1), gate (key down) are driven per voice by the host.
declare name "Faust Pluck";
declare author "keysynth";
declare license "AGPL-3.0-or-later";

import("stdfaust.lib");

freq = hslider("freq", 220, 20, 4000, 0.01);
gain = hslider("gain", 0.8, 0, 1, 0.01);
gate = button("gate");

decay      = hslider("[0]decay [unit:s][scale:log]", 4, 0.2, 20, 0.01);
brightness = hslider("[1]brightness", 0.5, 0, 1, 0.01);
pickPos    = hslider("[2]pick_position", 0.2, 0.05, 0.5, 0.01);
damping    = hslider("[3]release_damping [unit:s][scale:log]", 0.25, 0.02, 4, 0.01);
level      = hslider("[4]level [unit:dB]", 0, -40, 6, 0.1) : ba.db2linear : si.smoo;

trig = gate > gate';                       // rising edge of the gate = pluck
period = ma.SR / freq;

// Excitation: one period of noise, velocity-dependent brightness, comb-filtered by the pick position.
burst = no.noise * en.ar(0.0002, 1.0 / freq, trig) * gain
      : fi.lowpass(1, 600 + 12000 * gain * gain)
      <: _, de.fdelay(4096, pickPos * period) : -;

// Loop: fractional delay + one-zero lowpass (brightness) + loop gain for a T60 of `decay`
// (gate on) or `release_damping` (gate off).
t60 = select2(gate > 0, damping, decay) : si.smooth(ba.tau2pole(0.005));
loopGain = pow(0.001, 1.0 / (freq * t60));
b = 0.5 + 0.5 * brightness;                // 1 = no damping
filt(x) = b * x + (1 - b) * x';
loopDelay = max(1.5, period - 1 - (1 - b));  // feedback adds 1 sample, filt adds (1-b)
string = + ~ (de.fdelay4(4096, loopDelay) : filt : *(loopGain));

process = burst : string : fi.dcblocker : *(level);
