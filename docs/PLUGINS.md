# Plugins: hot-reloaded DSP (Faust JIT / C++ DLL)

How a user or an AI agent adds an instrument or effect without touching the engine. Design and invariants:
`docs/ARCHITECTURE.md` §10. Protocol messages: `docs/PROTOCOL.md` (`list_plugins`, `reload_plugin`, `plugin_status`).

Two paths, one host:

| | Faust `.dsp` (primary) | C++ `.cpp` → DLL |
|---|---|---|
| Use for | almost everything: synths, filters, modulation, physical models | heavy/hand-optimized C++ DSP, existing C++ code |
| Build | none — the engine JIT-compiles it (libfaust/LLVM) | `scripts/build_plugin.ps1 <name>` (MSVC) |
| Reload | save the file → ~0.1 s later it sounds | rebuild → the engine picks up the new DLL |
| Params | from the Faust UI (sliders etc.) | `ks_param_spec` table |
| Polyphony | engine-provided (freq/gain/gate) | your own |

The engine (`keysynth-engine`) watches `plugins/` (every 500 ms). Each plugin appears in the module catalog as
`plugin:<name>` and is used in patches like any built-in (`"type": "plugin:faust_pluck"`). Compile errors show up
in the UI (plugin toast) and as `log` events; the previous good version keeps playing.

## Layout

```
plugins/<name>/<name>.dsp      Faust source            (or)   plugins/<name>/<name>.cpp   C++ source
plugins/<name>/plugin.json     optional manifest (Faust)       plugins/<name>/*.h          local headers
plugins/<name>/*.dsp|*.lib     local Faust imports (hashed into the cache key)
plugins/<name>/demo.json       a patch using the plugin (convention, for ks-render)
plugins/.build/                gitignored: <name>-<hash>.dll, cache/ (Faust machine code), obj/
```

`<name>` = directory name = plugin id: `[a-z][a-z0-9_]{0,63}` (other directories are ignored). Ids and param ids
are part of the preset format: renaming breaks presets.

Examples: `fx_simple_tremolo` (Faust effect, menu + bargraph), `moog_vcf` (Faust effect, `ve.moog_vcf`, groups),
`faust_pluck` (Faust instrument, Karplus-Strong), `string_machine` (Faust instrument, Solina-style, stereo),
`cpp_bitcrusher` (C ABI effect, enum/int/read-only params, state blob).

## Faust instrument (template)

```faust
// plugins/my_synth/my_synth.dsp — save as UTF-8 *without* BOM
declare name "My Synth";
import("stdfaust.lib");

freq = hslider("freq", 440, 20, 20000, 0.01);   // Hz, set per voice (incl. pitch bend ±2 st)
gain = hslider("gain", 0.8, 0, 1, 0.01);        // velocity 0..1
gate = button("gate");                          // 1 while the key (or sustain pedal) holds the note

cutoff  = hslider("h:Filter/[0]cutoff [unit:Hz][scale:log]", 2000, 50, 16000, 1) : si.smoo;
release = hslider("h:Env/[1]release [unit:s][scale:log]", 0.3, 0.01, 5, 0.01);
level   = hslider("level [unit:dB]", -6, -40, 6, 0.1) : ba.db2linear : si.smoo;

env = en.adsr(0.005, 0.2, 0.7, release, gate);
process = os.sawtooth(freq) : fi.lowpass(2, cutoff) : *(env * gain * level);   // 1 or 2 outputs
```

```json
{ "name": "My Synth", "kind": "instrument", "category": "Synth", "polyphony": 8, "max_release_s": 6 }
```

Voice rules: one Faust instance per voice. Voice controls are found by label anywhere in the UI tree: `freq` (Hz)
or `key` (MIDI note), `gain` (0..1) or `vel`/`velocity` (0..127), `gate` (required). They are not params. A
re-struck or stolen voice gets `gate` = 0 for one sample, so envelopes retrigger on the rising edge. A voice is
freed when `gate` is 0 and its output stays below −90 dBFS for 2048 samples (or after `max_release_s`), so make
the release actually decay. Mono outputs are copied to both channels. Keep per-voice level modest (8 voices sum).

## Faust effect (template)

```faust
declare name "My Drive";
import("stdfaust.lib");
drive = hslider("drive [unit:dB]", 6, 0, 36, 0.1) : ba.db2linear : si.smoo;
mix   = hslider("mix", 1, 0, 1, 0.01) : si.smoo;
fx(x) = x * (1 - mix) + ma.tanh(x * drive) / sqrt(drive) * mix;
process = fx, fx;                       // 2 in / 2 out (stereo)
```

Accepted shapes: 2→2, 1→2 (input = mono sum), 2→1 (output copied), 1→1 (run as dual mono: two independent
instances). Effects need ≥ 1 input. `kind` defaults to *instrument* when the program has 0 inputs and a `gate`
control, else *effect*; set it in `plugin.json` to be explicit.

## plugin.json (Faust; all fields optional)

| key | default | meaning |
|---|---|---|
| `id` | dir name | must equal the directory name if given |
| `name` | `declare name` or dir name | display name |
| `kind` | inferred | `"instrument"` / `"effect"` |
| `category` | `"Plugin"` | catalog category |
| `polyphony` | `[nvoices:N]` in `declare options`, else 8 | voices, 1..32 |
| `max_release_s` | 10 | hard stop after note-off (instruments) |
| `tail_s` | 1 | tail reported for graph transitions (effects) |
| `faust_options` | `[]` | extra compiler flags, white-listed: `-vec -scal -dfs -fun -exp10 -mapp`, `-vs/-lv/-mcd/-ftz/-fm N` |

C++ plugins carry their metadata in the descriptor (no plugin.json).

## Parameters (Faust UI → ParamSpec)

| Faust | keysynth |
|---|---|
| `[id:x]` metadata | param id `x` (must be `[a-z][a-z0-9_]*`) |
| otherwise | snake_case of the path below the root group: `h:Env/Attack` → `env_attack` |
| `hslider`/`vslider`/`nentry` | Linear; `[scale:log]` (min > 0) → Log; integral min/max/init and step ≥ 1 → Int |
| `[style:menu{'A':0;'B':1}]` / `radio{..}` | Enum (labels; the value sent to Faust is mapped from the index) |
| `checkbox` / `button` | Bool (button: momentary, NonAutomatable) |
| `hbargraph` / `vbargraph` | ReadOnly (readout in telemetry, ≤ 30 Hz) |
| `[unit:Hz]`, `[hidden:1]` | unit, Hidden flag |
| innermost group (`h:Filter/...`) | param group; group order → `uiHints.groupOrder` |
| `[0]`, `[1]` label prefixes | Faust ordering only (stripped by the compiler) |

Duplicate ids (after snake_case) or invalid `[id:]` reject the plugin with a message naming both paths. `soundfile`
is not supported. Unused controls are optimized away by Faust (they won't appear). Smooth audible params in the DSP
(`si.smoo`): values change once per block.

## C++ plugin (C ABI)

Header: `sdk/include/keysynth/plugin_abi.h` (documented in place; read it first). Template: copy
`plugins/cpp_bitcrusher/cpp_bitcrusher.cpp`. Essentials:

- Export `ks_get_plugin()` returning a static `ks_plugin_descriptor` with `struct_size = sizeof(...)`,
  `abi_version = KS_PLUGIN_ABI_VERSION`, `id` = directory name, `kind`, a static `ks_param_spec[]`.
- Required: `create`, `destroy`, `prepare`, `reset`, `process`, `set_param`. Optional (NULL): `get_param` (for
  `KS_PARAM_READ_ONLY` readouts), `tail_samples`, `latency_samples` (keep 0), `save_state`/`load_state`.
- `process(inst, stereo, n, events, nevents)`: instruments get zeroed buffers, effects process in place; events are
  `ks_event` (note on/off, CC, bend, ...), sorted, offsets < n. Instruments do their own voice handling.
- `set_param` arrives on the audio thread at block start, only when a value changed (all once after `prepare`).
- `save_state` may run on the control thread while `process` runs: make it thread-safe (copy atomically).

Build: `scripts/build_plugin.ps1 my_fx` (`-Config Debug`, `-ExtraFlags '/arch:AVX2'`) → `plugins/.build/my_fx-<hash>.dll`
(~1–2 s; exit 3 = MSVC not found, 1 = compile error with the compiler output). The running engine loads the newest
build within ~0.5 s. Static CRT (`/MT`), `/W4`; only the C ABI crosses the boundary — no C++ types, no exceptions
out of the DLL.

## Real-time rules (both paths)

ARCHITECTURE §4 applies to everything that runs per block: no allocation, locks, file/console IO, exceptions, or
unbounded loops in `process`/`set_param`/Faust `compute`. Allocate in `prepare`. Faust code is RT-safe by
construction (no allocation in `compute`; `ks-tests` checks the LLVM IR of the examples); avoid `ffunction` calls
into non-RT C functions. Never produce NaN/Inf: the host zeroes such blocks and reports `faulted`.

Isolation is *lite*: a crash (access violation, stack overflow, ...) inside plugin code is caught (SEH), that
instance is muted and `plugin_status: faulted` is sent; fix the code and save/rebuild to get a fresh instance. A
plugin can still corrupt engine memory or hang the audio thread — plugins are trusted local code.

## Testing a plugin

```powershell
# Offline render (compiles plugins synchronously; Faust machine-code cache makes repeats fast)
build/bin/Release/ks-render.exe --patch-json '@plugins/faust_pluck/demo.json' --test-pattern --out renders/pluck.wav
build/bin/Release/ks-render.exe --patch-json '@plugins/faust_pluck/demo.json' --notes "A4:0:1.5:100" --out renders/a4.wav
python scripts/analyze_wav.py renders/a4.wav --start 0.1 --end 1.0      # pitch (should be 440 Hz), levels, tail
build/bin/Release/ks-render.exe --list-modules                            # catalog incl. plugin params
build/bin/Release/ks-tests.exe "[plugins]"                                # host tests ([faust], [dll] subsets)
```

`ks-render` loads only the plugins the patch references (all for `--list-modules`); errors are printed as
`plugin <name>: error: ...`. Check: no NaN/Inf, peak ≤ 0 dBFS, not silent, tails decay (the same criteria as the
factory render suite, `docs/TESTING.md`). Live: run `keysynth-engine --no-audio` (or with audio), open the UI, add
`plugin:<name>`, edit and save the source — the toast shows `compiling…` → `loaded vN` or the error.
Over the protocol: `{"type":"reload_plugin","name":"faust_pluck"}` forces a recompile; `list_plugins` lists states.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `libfaust (faust.dll) not found` | `scripts/fetch_faust.ps1` (installs `.tools/faust`), or set `KS_FAUST_DIR` to a Faust 2.88 install |
| `starts with a UTF-8 BOM` | save the `.dsp` as UTF-8 without BOM (PowerShell 5 `Set-Content -Encoding utf8` adds one) |
| `<file>.dsp:12 : ERROR : ...` | Faust compiler message; line numbers refer to your file |
| `instrument has no 'gate' control` | add `gate = button("gate");` and use it, or set `"kind": "effect"` |
| `duplicate param id 'x'` / `invalid [id:...]` | add/rename `[id:...]` metadata |
| `effect must have 1-2 inputs and 1-2 outputs` | fix `process` arity (`_,_ : ...` for stereo) |
| `not built: run scripts/build_plugin.ps1 <name>` | C++ plugin without a DLL in `plugins/.build/` |
| `descriptor id ... must equal the plugin directory name` | set `id` in the descriptor to the directory name |
| `faulted: crashed (exception 0xC0000005)` | null/out-of-bounds access in plugin code; fix, rebuild (new instance) |
| `faulted: produced NaN/Inf` | unstable filter/feedback or division by zero; clamp params, add `ma.EPSILON` |
| patch shows `unknown module type 'plugin:x'` | plugin not compiled (yet) or failed; the slot is kept and loads when the plugin compiles |
| voice never ends / CPU grows | release doesn't decay below −90 dB; lower `max_release_s` or fix the envelope |

Compile times (this machine, Faust 2.88 / LLVM 17, scalar): tremolo ≈ 20 ms, pluck ≈ 50 ms, moog_vcf ≈ 50 ms,
string_machine ≈ 120 ms; machine-code cache hits ≈ 1 ms. C++ DLL build ≈ 1.2 s (vcvars + cl + link).
