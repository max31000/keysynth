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
- `rhythm` (optional): `{ "node": 9, "kit": { params of the `drums` module }, "pattern": "presets/patterns/<x>.json" }`
  — the drum-sequencer kit (RhythmNode, ARCHITECTURE §5.3) and the pattern loaded with the preset. A preset without
  `rhythm` keeps the current rhythm section (kit + pattern) and, without `tempo`, the current tempo, so a running
  groove survives sound changes. Written back always.
- `zone.channel`: 0 = omni, 1–16 = specific channel. Keys are MIDI note numbers (C4 = 60).
- `zone.transpose`: semitones, applied after the key-range filter (range is on the physical key).
- `params`: plain units as defined by the module's `ParamSpec` (see engine catalog via protocol `get_catalog`,
  or `ks-render --list-modules`). Enum params are numeric indices.
- `state`: module-specific non-param data (e.g. `sampler`: `{ "sfz": "assets/samples/salamander/x.sfz" }`,
  `fm`: `{ "syx": "...", "voice": 10 }`). Paths relative to repo root or absolute.
- Splits: multiple layers with disjoint key ranges. Layers: overlapping ranges.

Files: `presets/factory/<category-slug>/<preset-slug>.json` (read-only), `userdata/presets/<slug>.json`.
Every factory preset must pass the render test suite (`docs/TESTING.md`).

## Patterns (`presets/patterns/*.json`, `userdata/patterns/*.json`)

Drum-sequencer patterns (ARCHITECTURE §8). Same leniency rules as patches.

```json
{
  "format": 1,
  "name": "Billie Jean Groove",
  "description": "Tight 80s pop-funk groove. Reference: Michael Jackson — Billie Jean.",
  "tempo": 117,
  "time_sig": [4, 4],
  "steps_per_beat": 4,
  "bars": 1,
  "swing": 0.0,
  "accent_amount": 0.5,
  "kit": "linn",
  "tracks": [
    { "name": "Kick",   "note": 36, "steps": "x.......x......." },
    { "name": "Snare",  "note": 38, "steps": "....x.......x..." },
    { "name": "Closed", "note": 42, "steps": "x.x.x.x.x.x.x.x.", "mute": false }
  ],
  "accent": "x.......x......."
}
```

- `time_sig` `[num, den]`: num 1..16, den 2/4/8/16. A step is `1 / steps_per_beat` of a *denominator* beat
  (`steps_per_beat` 1..8, default 4 = 16ths in x/4). Steps per bar = `num × steps_per_beat`;
  pattern length = `bars` (1..4) × steps per bar, at most 256.
- `tracks` (≤ 16): `note` = MIDI note sent to the kit (GM drum map, see the `drums` module), `mute` optional.
  `steps`: a string (one char per step: `.` `-` `_` off, `x` = 100, `X` = 127, `o` = 50 ghost, `1`–`9` =
  n × 127 / 9; spaces and `|` ignored) or an array of velocities 0..127. Short rows are padded, long ones truncated.
- `accent`: string (`x` = accent) or array of 0/1, one per step; accented steps get
  `velocity + accent_amount × 64` (clamped to 127).
- `swing` 0..1: every second step is delayed by `swing × ½ step` (0.33 ≈ triplet shuffle, 1 = dotted).
- Optional `tempo` (BPM), `kit` (`"808"`, `"909"`, `"linn"`, `"industrial"` or index 0..3) are applied when the pattern
  is loaded; the engine writes back steps as arrays.
