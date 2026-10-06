# Patch / preset format

One patch per JSON file. `format` is bumped only for breaking changes (migrations in `engine/src/preset/Migrations.cpp`).
Loading is lenient: unknown keys/params are ignored (logged), missing params take the module default.

```json
{
  "format": 1,
  "meta": {
    "name": "Light My Fire Organ",
    "category": "Organ",
    "tags": ["doors", "60s", "combo"],
    "description": "Vox Continental-style combo organ. Reference: The Doors — Light My Fire.",
    "author": "factory"
  },
  "tempo": 120,
  "layers": [
    {
      "name": "Organ",
      "zone": { "key_lo": 36, "key_hi": 108, "vel_lo": 1, "vel_hi": 127, "transpose": 0,
                "channel": 0, "volume_db": 0, "pan": 0, "mute": false, "solo": false, "sustain": true },
      "instrument": { "type": "combo", "params": { "voicing": 0, "foot_16": 0.0, "foot_8": 1.0 }, "state": {} },
      "fx": [
        { "type": "drive", "bypass": false, "params": { "amount": 0.2 } },
        { "type": "reverb", "bypass": false, "params": { "mix": 0.18, "size": 0.6 } }
      ]
    }
  ],
  "master": { "volume_db": -3, "fx": [] }
}
```

Field rules:
- `node`: optional stable uint32 on each layer, instrument and fx entry (e.g. `"node": 3`). Assigned by the engine
  when missing; `0` is reserved for the master chain. Hand-written presets may omit them.
- `rhythm` (optional): `{ "kit": { params of a drums module }, "pattern": "presets/patterns/<x>.json" }`.
- `zone.channel`: 0 = omni, 1–16 = specific channel. Keys are MIDI note numbers (C4 = 60).
- `zone.transpose`: semitones, applied after the key-range filter (range is on the physical key).
- `params`: plain units as defined by the module's `ParamSpec` (see engine catalog via protocol `get_catalog`,
  or `ks-render --list-modules`). Enum params are numeric indices.
- `state`: module-specific non-param data (e.g. `sampler`: `{ "sfz": "assets/samples/salamander/x.sfz" }`,
  `fm`: `{ "syx": "...", "voice": 10 }`). Paths relative to repo root or absolute, inside the allow-listed roots
  (ARCHITECTURE §11). `sampler` also finds `assets/...` paths in `$KS_ASSETS_DIR` and in the main checkout's `assets/`
  (docs/TESTING.md). Changing `state` rebuilds (reloads) the module; params never do.
- Splits: multiple layers with disjoint key ranges. Layers: overlapping ranges.

Files: `presets/factory/<category-slug>/<preset-slug>.json` (read-only), `userdata/presets/<slug>.json`.
Every factory preset must pass the render test suite (`docs/TESTING.md`).
