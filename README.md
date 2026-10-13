# keysynth

Software synthesizer for MIDI keyboards. C++20 / JUCE engine (ASIO / CoreAudio, low latency) with a web UI.

- Engines: virtual analog, DX7-style FM, tonewheel and combo organs, electric piano, SFZ sampler, drum synth
- Effects: chorus, ensemble, phaser, flanger, delay, reverb, drive, rotary, tremolo, compressor, EQ
- Layers and keyboard splits, JSON presets, metronome and drum sequencer
- Hot-reloadable DSP plugins (Faust or C++)

Install: [keysynth-setup.exe](https://github.com/max31000/keysynth/releases/latest/download/keysynth-setup.exe) (Windows, per-user) or [keysynth-macos.dmg](https://github.com/max31000/keysynth/releases/latest/download/keysynth-macos.dmg) (macOS 13+, Apple Silicon; not notarized: first start via System Settings › Privacy & Security › «Open Anyway»). Run from source: `scripts/start.ps1` / `scripts/start.sh` (engine + UI in the browser; `start-dev` for UI work). Docs: [docs/](docs/). License: AGPL-3.0-or-later.
