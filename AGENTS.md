# Agent guide

Read before changing anything. Keep this file short; details live in `docs/`.

## Read first
- `docs/ARCHITECTURE.md` — structure, threads, real-time rules, module API. Source of truth.
- `docs/PROTOCOL.md` — engine ⇄ UI WebSocket protocol.
- `docs/PRESETS.md` — patch JSON schema.
- `docs/TESTING.md` — how to build, run, test, render and analyze audio.
- `docs/PLUGINS.md` — hot-reloaded DSP plugins (Faust / C++ DLL).
- `docs/STATUS.md` — what is done, in progress, next. Update it when you finish a chunk of work.

## Workflow rules
1. **Architecture first.** Changes that add a subsystem, change threading, the module API, the patch format or the
   protocol: update `docs/ARCHITECTURE.md` (or the relevant doc) first, then code.
2. **Review after every big batch.** After a batch of code (new module, subsystem, >~300 changed lines), run a review
   by a separate subagent (fresh context) against: real-time rules (ARCHITECTURE §4), module/API conventions,
   tests, docs consistency. Fix findings or record why not in `docs/STATUS.md`.
3. **Tests are part of the change.** New DSP/engine code ships with Catch2 tests; new presets must pass the render
   test suite. Don't claim something works without running build + tests (and, for sounds, `ks-render` + analysis).
4. **No duplicated knowledge.** Facts go in one doc and are referenced elsewhere. Prompts to subagents should point
   to docs, not restate them.
5. **Keep modules isolated.** An engine/effect lives in its own directory and talks to the rest only through the
   `Module` API. No cross-engine includes except `core/` and `dsp/`.
6. Real-time code: no allocation, locks, IO, logging or exceptions on the audio thread. Reviewers reject violations.
7. Param ids and preset format are compatibility surfaces — renames need a migration.
8. **One primitive per concept in `engine/src/dsp/`.** Before adding a file/function there, grep for an existing
   one (delay lines, one-poles, SVFs, oversamplers…) and extend it. Never reuse a name with different semantics.
9. **Parallel work in worktrees:** before your final commit, `git merge main` into your branch, resolve, rebuild and
   rerun tests, so the lead's merge is a fast integration, not a conflict hunt. Never run a non-disconnected
   CMake configure concurrently with other agents against the shared `.deps/` (use `scripts/configure.ps1`).
10. **Stop what you start.** Every background process you launch (mock, Vite, dev engine, test servers) is stopped
    before you finish (`scripts/stop-dev.ps1`; it never touches the installed app). The dev mock uses its own port
    (7441); never run anything on the engine's 7341/7340 — the owner's installed keysynth needs them. The Browser
    pane's `preview_start` reads the **main checkout's** `.claude/launch.json`: from a worktree start servers yourself.

## Commands (Windows, PowerShell)
```
scripts/configure.ps1          # cmake configure (build/), shared dependency cache in .deps/
cmake --build build --config Release --target keysynth-engine ks-render ks-tests
build/bin/Release/ks-tests.exe
build/bin/Release/keysynth-engine.exe            # real audio (ASIO)
build/bin/Release/keysynth-engine.exe --no-audio # null device, for UI work
scripts/start.ps1 / scripts/start-dev.ps1        # launchers (play / UI dev), see docs/TESTING.md
cd ui; npm install; npm run dev                   # UI dev server (http://localhost:5173)
cd ui; npm test
```
macOS (Apple Silicon): `scripts/configure.sh`, `cmake --build build --target ...`, `build/bin/ks-tests`,
`scripts/start.sh` / `start-dev.sh` / `stop-dev.sh`, `scripts/fetch_faust.sh`, `scripts/build_plugin.sh`,
`scripts/build_app_macos.sh` (docs/TESTING.md *macOS*). Both platforms ship the same features and version: a change
that touches `engine/src/platform/` or `app/` needs both branches (Windows / macOS); CI builds macOS on every PR.
Parallel agents: use your own build dir (`-B build-<task>`) to avoid clobbering.

## Conventions
- C++20, MSVC and Apple clang, warnings as errors in `engine/` code (not deps). `namespace ks`. Files `PascalCase.h/.cpp`.
- Param ids `snake_case`; module typeIds lowercase (`va`, `epiano`, `plugin:<id>`).
- UI: TypeScript strict, React function components, Zustand store, protocol types in `ui/src/protocol/`.
- Commits: conventional (`feat(va): ...`, `fix(engine): ...`).
