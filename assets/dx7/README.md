# DX7 banks

- `keysynth-fm-factory.syx` — 32-voice DX7 bulk dump of the `fm` factory presets (original keysynth voices, voices
  1–11; 12–32 are INIT VOICE). Same licence as the repository (AGPL-3.0-or-later). Load it in the `fm` module with
  state `{ "syx": "assets/dx7/keysynth-fm-factory.syx", "voice": 0 }` or on any DX7-compatible synth.

Third-party DX7 cartridges (including Yamaha ROM banks) are not committed: their redistribution terms are unclear.
Drop your own `.syx` files here or under `userdata/` (paths outside the allow-listed roots are rejected).
