# Patch / preset format

One patch per JSON file. `format` is bumped only for breaking changes (migrations in `engine/src/preset/Migrations.cpp`;
the engine always writes the current format). Format history:
- **2** — every tempo-syncable param is one `sync` enum over the shared note-division list (ARCHITECTURE §7).
  Format 1 → 2: `delay`/`phaser`/`flanger` `sync` ≥ 1 shift by one ("4/1" was added); `tremolo` `sync` (bool) +
  `division` and `va` `lfoN_sync` (bool) + `lfoN_division` merge into `sync` / `lfoN_sync` (0 = off). Old
  `division` ids are merged in any format; a file without `format` is read as the current one.
Loading is lenient: unknown keys/params are ignored (logged), missing params take the module default.

```json
{
  "format": 2,
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
- `master.volume_db`: −96..+12 dB, applied after the master fx but before trailing `limiter` slots (ARCHITECTURE
  §5.3). Factory presets are normalized with it to −16 LUFS / ≤ −1 dBTP (`scripts/loudness.py`, docs/TESTING.md);
  `zone.volume_db` has the same range. Out-of-range values are clamped with a load warning.
- `zone.channel`: 0 = omni, 1–16 = specific channel. Keys are MIDI note numbers (C4 = 60).
- `zone.transpose`: semitones, applied after the key-range filter (range is on the physical key).
- `params`: plain units as defined by the module's `ParamSpec` (see engine catalog via protocol `get_catalog`,
  or `ks-render --list-modules`). Enum params are numeric indices.
- `state`: module-specific non-param data (e.g. `sampler`: `{ "sfz": "assets/samples/salamander/x.sfz" }`,
  `fm`: `{ "syx": "...", "voice": 10 }`). Paths relative to repo root or absolute, inside the allow-listed roots
  (ARCHITECTURE §11). `sampler` also finds `assets/...` paths in `$KS_ASSETS_DIR` and in the main checkout's `assets/`
  (docs/TESTING.md). Changing `state` rebuilds (reloads) the module; params never do.
- Splits: multiple layers with disjoint key ranges. Layers: overlapping ranges.
- `layers[].midi` (optional): ordered MIDI processors in front of the instrument (ARCHITECTURE §17), same slot shape
  as `fx` (`node`, `type`, `bypass`, `params`, `state`), ≤ 8. Types `arp`, `chord`, `hold`. Absent = none; the
  engine writes the key only when the chain is not empty (no format bump).
  ```json
  "midi": [ { "type": "arp", "params": { "mode": 0, "octaves": 2, "sync": 13, "gate": 0.5, "latch": true } } ]
  ```

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

## Songs (`presets/songs/*.json`, `userdata/songs/*.json`)

Setlist entries (ARCHITECTURE §18). Factory songs are read-only; user songs live in the active profile and sync to
the account (`/api/songs`, cloud-api spec). Same leniency as patches: unknown keys are kept verbatim (written back
unchanged, so newer fields survive older engines), wrong-typed known fields are ignored with a warning.

```json
{
  "format": 1,
  "id": "riders-on-the-storm",
  "title": "Riders on the Storm",
  "artist": "The Doors",
  "tempo": 104,
  "time_sig": [4, 4],
  "key": "E minor",
  "notes": "Rhodes intro, drums from bar 5.",
  "control_keys": { "enabled": true, "prev": 21, "next": 22 },
  "sections": [
    { "name": "Intro", "preset": "presets/factory/e-piano/riders-on-the-storm-rhodes.json", "drums": false },
    { "name": "Verse", "preset": "presets/factory/e-piano/riders-on-the-storm-rhodes.json",
      "pattern": "presets/patterns/jazz-swing.json", "drums": true, "notes": "Em7 - A" },
    { "name": "Solo", "preset": "warm-rhodes", "tempo": 106 }
  ]
}
```

| field | rules |
|---|---|
| `format` | 1. Bumped only for breaking changes. |
| `id` | slug of the file name (`^[a-z0-9]+(-[a-z0-9]+)*$`, ≤ 64). The file name wins; the engine writes it back. |
| `title` (required), `artist`, `key`, `notes` | strings; `title` 1..200 chars, `notes` ≤ 2000. `key` is free text ("E minor"). |
| `tempo` | BPM 20..400 (optional; default: keep the current tempo). |
| `time_sig` | `[num, den]` as in patterns (optional). |
| `control_keys` | optional per-song override of the global control keys: `{ enabled, prev, next }`, `prev`/`next` MIDI note 0..127 or `null`. Absent = the global setting. |
| `sections` | 1..64 entries, played in order. |
| `sections[].name` | 1..60 chars (required). |
| `sections[].preset` | logical path under `presets/factory/` or `userdata/presets/`, or a bare user preset slug (= `userdata/presets/<slug>.json`, follows the account's synced presets). Absent = keep the current sound. |
| `sections[].pattern` | logical path under `presets/patterns/` or `userdata/patterns/`; `""` = empty pattern; absent = keep. |
| `sections[].drums` | `true` = drums on + transport playing; `false` = transport stopped; absent = unchanged. |
| `sections[].tempo` | BPM override for this section (else the song `tempo`). |
| `sections[].notes` | free text ≤ 2000 chars (shown on the remote view). |

Reserved (documented for later, not implemented; kept verbatim): song `notation` `{ "musicxml": "<logical path>" }`,
song `backing` `{ "audio": "<logical path>", "offset_ms": 0 }`, section `bars` (length for auto-advance), section
`start_bar` (score position), section `notation_part`. Engines ignore them today.
