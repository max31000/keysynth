# Faust toolchain setup (Windows, verified 2026-10-06)

Fetch: `scripts/fetch_faust.ps1` (idempotent; `-Force` to redo, `-Version x.y.z` for another release).
It downloads `Faust-2.88.0-win64.exe` (113,237,948 bytes) to `.deps/downloads/` and unpacks the NSIS installer with
`7z x` into `.tools/faust/`. **The installer is never run.** Falls back to `py7zr` if 7-Zip is missing (untested path;
7-Zip at `C:\Program Files\7-Zip\7z.exe` was used). The `$PLUGINSDIR` and uninstaller are removed afterwards.
Both `.deps/` and `.tools/` are git-ignored.

## Result
- `.tools/faust/bin/faust.exe --version` -> `FAUST Version 2.88.0`, "Build with LLVM version 17.0.6".
  Embedded backends include C, C++, old C++, LLVM IR, Interpreter, WebAssembly, Rust, Cmajor, JSFX, etc.
- Size on disk: ~717 MB (`bin` 39 MB, `lib` 594 MB, `include` 8 MB, `share` 77 MB).
- MSVC runtime DLLs (`msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`) ship next to `faust.exe` in `bin/`.
  The `faust2xxx` scripts in `bin/` are shell scripts (not usable from PowerShell; not needed).

## Layout
| What | Path |
|---|---|
| Compiler | `.tools/faust/bin/faust.exe` |
| Architecture headers | `.tools/faust/include/faust/` (`dsp/`, `gui/`, `audio/`, `midi/`, `osc/`, ...) |
| poly-dsp | `.tools/faust/include/faust/dsp/poly-dsp.h` (also `poly-llvm-dsp.h`, `poly-interpreter-dsp.h`) |
| libfaust API | `include/faust/dsp/libfaust.h`, `llvm-dsp.h`, `interpreter-dsp.h`, `libfaust-c.h`, `llvm-dsp-c.h` |
| DSP libraries (`stdfaust.lib`, `oscillators.lib`, ... 53 `.lib`) | `.tools/faust/share/faust/` |
| Architecture files (`minimal.cpp`, `*.cpp` targets) | `.tools/faust/share/faust/` (no separate `architecture/` dir; they sit next to the `.lib` files, 170 entries) |

`faust.exe` finds `share/faust` relative to itself; no `-I` / `FAUST_LIB_PATH` was needed.

## libfaust libraries (`.tools/faust/lib/`)
| File | Size | What |
|---|---|---|
| `faust.dll` | 38.6 MB | Dynamic libfaust **with LLVM statically inside** (exports `createDSPFactoryFromString` (LLVM) and `createInterpreterDSPFactoryFromString`) |
| `faust.lib` | 0.5 MB | Import library for `faust.dll` |
| `libfaust.lib` | 113.7 MB | Static libfaust (references LLVM symbols but LLVM itself is not merged: needs separate LLVM 17 libs to link the LLVM backend) |
| `libfaustwithllvm.lib` | 466.8 MB | Static libfaust with LLVM 17.0.6 merged in (self-contained) |
| `libOSCFaust.lib`, `libHTTPDFaust.lib` | ~1.5 MB each | OSC / HTTPD control static libs |

For runtime hot-reload (docs/PLUGINS.md), linking `faust.lib` + shipping `faust.dll` is the simplest option (no 467 MB
static link). Static libs are MSVC-built; check CRT (/MD vs /MT) compatibility before linking.

## Codegen check
```
faust -lang cpp -cn OscDsp -o osc.cpp osc.dsp          # 198 lines, class OscDsp : public dsp, compute()
faust -a minimal.cpp -i -cn OscDsp -o osc_min.cpp osc.dsp  # 4067 lines, architecture headers inlined
```
with `osc.dsp` = `import("stdfaust.lib"); process = os.osc(hslider("freq",440,20,20000,1)) * hslider("gain",0.5,0,1,0.01);`.
Both succeeded. The generated C++ was not compiled with MSVC in this check.
