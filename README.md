# keysynth

Software synthesizer for MIDI keyboards. C++20 / JUCE engine (ASIO, low latency) with a web UI.

- Engines: virtual analog, DX7-style FM, tonewheel and combo organs, electric piano, SFZ sampler, drum synth
- Effects: chorus, ensemble, phaser, flanger, delay, reverb, drive, rotary, tremolo, compressor, EQ
- Layers and keyboard splits, JSON presets, metronome and drum sequencer
- Hot-reloadable DSP plugins (Faust or C++)

Run: `scripts/start.ps1` (engine + UI in the browser; `scripts/start-dev.ps1` for UI work). Docs: [docs/](docs/). License: AGPL-3.0-or-later.
