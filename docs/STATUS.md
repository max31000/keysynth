# Status

Update when a chunk of work lands. Newest first inside each section.

## Done
- macOS menu-bar widget (`platform/MenuBar_mac.mm`, ARCHITECTURE §16 *macOS*): native status item replacing JUCE's
  (whose macOS menu never opened): status popover (engine / audio / CPU + output meters / preset / MIDI / clients /
  cloud, restart audio, at-login switch, log, quit) + right-click menu. Checked by rendering (light / dark) and a
  start/quit run; clicking through it is a manual check.
- macOS port (Apple Silicon, macOS 13+), same features as Windows (ARCHITECTURE §1, §16 *macOS*; TESTING *macOS*):
  CoreAudio/CoreMIDI (default buffer 128), POSIX/Cocoa platform layer (per-user 0600 token key file, NSURLSession relay
  socket, libproc port owner, flock + socket single instance, Apple Events, SMAppService login item, signal-based
  plugin fault guard, FPCR flush-to-zero), Faust (official arm64 libfaust), C++ plugins as `.dylib`,
  `keysynth.app` menu-bar app + dmg (`scripts/build_app_macos.sh`, ad-hoc signed), `.sh` launchers, OS-aware web
  UI downloads, `release.yml` (one release for both OSes) + `engine-macos.yml` (PR CI). All `ks-tests` pass on
  macOS with nothing skipped. Fixes found on the way that also mattered elsewhere: `resolveAllowed` rejected every
  absolute POSIX path, aligned `operator new` (RT checks) rejected alignments < 8 on non-MSVC, duplicate
  `platform::currentProcessId`. Manual checks still open (need a person at the Mac): menu-bar menu in light/dark,
  «Open at Login», `keysynth://` link, first start of a downloaded dmg, microphone prompt, web UI in Safari.
- v0.5.0 presets +73 (keys/acoustic: piano, EP, organ, bells, brass, strings, choir, splits) → 203 factory presets,
  all at −16 LUFS. Synth/FX preset batch (~65) pending (agent launch blocked by the permission classifier).
- v0.4.0 MIDI tools per layer (`arp`, `chord`, `hold`; module kind Midi) + songs/setlists (`song_*`, control keys,
  `/api/songs` sync, «Пульт» song mode, 6 factory songs).
- v0.3.0 effects `vocoder` (+ engine audio input bus), `wah`, `echo` (Echorec), `tapestop`; OnePoleSmoother in double.
- Per-layer MIDI tools + setlists (ARCHITECTURE §17, §18): `ModuleKind::Midi` + `processMidi`, `layer.midi[]`
  (no format bump), `arp` (6 modes, octaves, NoteDivision sync / free rate, gate/legato, swing, latch, retrigger,
  velocity modes; transport grid or own sample clock), `chord` (12 types + memory, voicings, inversions,
  `chord_learn`), `hold` (sustain / latch / pedal); bypass and chain edits release notes. Songs: `song/` model +
  `songs::` store, `song_*` protocol + `song_state`, section switching (preset → meter → pattern → tempo → drums),
  keyboard control keys (global in settings.json or per song), 6 factory songs (`presets/songs/`), sync of
  `userdata/songs/` via `/api/songs` (server: shared DocumentEndpoints, migration 002). UI: MIDI chain in the rack,
  Learn button, songs drawer/editor/bar, «Пульт» song mode, mock support. 6 factory presets (arp / chord).
  Tests: `[midi]`, `[songs]`, relay trust table, server SongEndpointTests, 5 Vitest files.
  Review notes kept open: a full 512-event MIDI buffer can drop a processor's note-off (state already updated);
  the fading instrument in a crossfade only loses note-ons from shared processors (no test); factory arp presets
  ship with latch off (render test requires decaying tails); deploy the API before engines that sync songs
  (older APIs answer 404 → treated as "no songs").
- Effects v2 + audio input (ARCHITECTURE §6 *Audio input*, §7; PROTOCOL *Audio input*):
  `vocoder`, `wah`, `echo` (Binson Echorec), `tapestop`; `dsp/EnvelopeFollower`; device input bus
  (`ProcessContext::input`, `Engine::process(inputs…)`, `set_audio_input`, settings, `telemetry.meters.input`, UI
  audio-dialog input selector + meter); tests `test_fx2.cpp` (`[fx2]`), audio-host / protocol / trust cases; 9 factory
  presets (vocoder ×3, Echorec ×2, wah ×3, tape stop). Not verified live: a real mic on the UR22C (the fake ASIO
  driver test covers open/persist/meter/rollback). Review (opus) fixed: input selection kept on a failed device
  switch / cleared on a startup fallback device, vocoder NaN-input guard + gated HF pass, echo wow excursion capped,
  tape-stop delay cap, rule-8 smoother reuse. Not done (accepted): no tail test for echo swell >= 0.93 (tail capped
  at 120 s by design); wah +9.5 dB resonance makeup relies on the preset/master limiter at extreme Q; set_audio_input
  shape is validated in the handler and defensively again in AudioHost.
- Fix: `dsp::OnePoleSmoother` keeps its state in double — the float version stalled a few samples short of large
  targets (delay times in samples) and then snapped, so `delay` time changes clicked at the end of the glide.
- v0.2.0 remote control via relay (spec docs/specs/2026-10-12-remote.md): server RelayHub `/api/relay`
  (R-A), engine relay host over WinHTTP WebSocket with remote trust (R-B), UI RelayTransport + touch «Пульт»
  view (R-C), nginx WS location (mvv42-infra PR #3). Engine E2E against the real API: `KS_TESTS_ENGINE=1`.
- Remote R-B (engine as relay host, spec `docs/specs/2026-10-12-remote.md` §10, ARCHITECTURE §15 *Relay host*):
  `platform/WinHttpWebSocket`, `cloud/RelayClient`, `Trust::Remote`, virtual clients `r:<cid>` + `control/RemoteRouter`
  (envelopes, lossy, rates, pause); `[relay]` tests; `scripts/dev_relay_server.py` + `scripts/dev_relay_e2e.py`
  (passes, also with `--idle 50`). Open: PROTOCOL.md `trust:"remote"` row / Relay section (R-A owns PROTOCOL),
  real mvv42.ru relay (R-A) not tried yet. Known/accepted: a WinHTTP handle closed by abort() may be used once more
  by the other socket thread (returns an invalid-handle error).
- Port-conflict incident fix (2026-10-12; a leftover dev mock on 7341 blocked the installed v0.1.0 and the platform
  page drove the mock): ControlServer checks the listener table before binding (ix sets `SO_REUSEADDR`), owner
  classification (`platform/PortOwner`, `control/PortConflict`), tray balloon/tooltip + 3 s retry instead of the
  fatal box (ARCHITECTURE §11, §16); `hello.engine` identity, UI refuses non-keysynth servers (mock only in dev,
  `MOCK` badge, no cloud link to it); mock on 7441, `dev:mock`, `start-dev.ps1 -Mock`, `stop-dev.ps1`, AGENTS rule
  10; audio dialog / top bar layout (no wrapped buttons, long device names, 1024–1440 px); terse UI texts; samples
  not downloaded (not linked / offline) are a notice + sampler-panel «Скачать сэмплы», not an error (offline
  retries every 30 s). Tests: `[port]` (fake listener), sampler notice/offline/retry, UI identity states.
  Not verified live: the tray balloon itself (a dev `--tray` run was checked via its log only) and the real ASIO
  device (the installed app was not touched).
- Cloud live (2026-10-12): `https://mvv42.ru/keysynth/` (frontend.yml), `keysynth-api` on 127.0.0.1:5020
  (backend.yml), release v0.1.0 with `keysynth-setup.exe` (release-windows.yml, first run green on the GitHub
  runner), nginx app conf + `_files` X-Accel location (mvv42-infra PR #1), DB `keysynth`, secrets synced, auth app
  row (`invite`, anonymous), samples uploaded to `/var/lib/keysynth/samples` (11 644 files, 3.62 GB, index.json).
- Cloud WP-C (Windows desktop packaging, ARCHITECTURE §16): `keysynth-app` (windows-subsystem build of the engine,
  log to `<data>/logs/engine.log`), tray icon + menu (open web/local UI with mvv42.ru health fallback, restart
  audio, autostart toggle, quit), single instance (mutex + open/quit events, `--tray`, `--no-tray`, `--quit`,
  `keysynth://`), version + icon resource (`KS_VERSION`, `assets/icon` from `scripts/make_icon.py`), bundled
  Faust lookup, Inno Setup per-user installer (`installer/keysynth.iss`, ~16 MB), `scripts/build_installer.ps1`,
  `.github/workflows/release-windows.yml`, `configure.ps1 -Generator auto`. 4 `[desktop]` tests; installer smoke
  test (install, tray menu, second instance, protocol launch, upgrade over a running engine, uninstall) passed.
- Cloud WP-B (engine): AppPaths installed/dev modes + logical paths + per-account profiles; Origin/Host session
  trust (`control/Trust`, web default-deny); CloudClient (JUCE/WinINet behind `HttpTransport`, DPAPI token in
  `cloud.json`, link/unlink/401 drop, `--cloud-api`); SampleService (manifest+ETag, jobs, 2 workers, Range/If-Range
  resume, SHA-256, strict path rules, refcounted delete, sampler auto-fetch with `loading`=1 + `resourcesChanged`);
  SyncService (contract §3.4, conflicts, tombstones, per-pass token binding); `scripts/dev_cloud_server.py`; mock
  engine cloud messages. 22 `[cloud]` tests (fake API) + hidden `[cloud-live]`; E2E against the dev server.
  Fresh-context review: fixed cross-account sync race, 401-vs-relink race, partial-cache loads, message-thread
  cache walk, reserved slugs, trailing-dot logical segments, manifest NTFS-case/`.part` collisions, inline tasks
  after stop.
- Cloud WP-A `server/` keysynth-api (.NET 8): devices (cookie mint/list/delete, device me), samples manifest +
  X-Accel files, presets (revisions, If-Match/If-None-Match, tombstones), rate limits, migrations; xUnit unit +
  WebApplicationFactory tests (in-memory repos), MySQL repository contract tests via Testcontainers (skipped
  without Docker, run in CI); `server/Dockerfile`, `.github/workflows/backend.yml` (inline copy of infra
  deploy-dotnet-api.yml). Review fixes applied (cookie endpoints reject any bearer identity, dev file serving only
  in Development, `If-Match: *`). Deferred review notes: DELETE on a tombstone bumps the revision again (per
  contract wording); no per-IP limiter before auth (bad bearers cost a DB lookup → rely on nginx `limit_req`);
  provisioning cache may return a deleted user id for ≤ 1 h (users are never deleted by the API); migrations take
  no lock (single instance, old container stopped first); no MySQL concurrency tests yet (limits / 412 races).
  Not yet verified against MySQL locally (no Docker on the dev box) — first CI run is the check.
- Cloud WP0 contracts: PROTOCOL.md *Cloud & samples* (trust/Origin/Host, link, sync, samples jobs, auto-fetch),
  `docs/specs/2026-10-12-cloud-api.md` (REST, DB, index/manifest, AppPaths, cache.json, cloud.json), TS types.
- Audio robustness after driver-side buffer/rate changes (bug: UR22C buffer change broke sound until restart).
  Root cause (code reading, JUCE 8.0.15): `ASIOAudioIODevice::timerCallback` (reset request) ignores a failed
  reopen and restarts callbacks anyway → "playing" device with no callbacks; also reads the preferred size from the
  old driver instance before reloading it. AudioHost now: re-prepares on any new rate/buffer, detects failed restarts
  / stalls (> 0.5 s logged, > 1.5 s reopen) / vanished device and reopens with the driver's current settings (backoff), never forces a stale
  saved ASIO size (`bufferExplicit`), logs device events to `userdata/logs/audio.log` + notify toasts, protocol
  `restart_audio` + UI "Restart audio". Fake ASIO device tests (`[audio]`). Needs confirmation on the UR22C.
- Consistency: one `sync` note-division enum (dsp/NoteDivision.h) for all tempo-synced modules, patch format 2 +
  migrations; master volume before limiter; all 115 factory presets at −16 LUFS ±1, ≤ −1 dBTP
  (`scripts/loudness.py --check`, docs/research/LOUDNESS.md); ks-render `--cc`/`--bend`.
- Integration: ASIO panel button + latency warning UX (`open_audio_panel`), launchers (scripts/start.ps1,
  start-dev.ps1), configure.ps1 lock/auto-disconnect, tabbed large module panels (`uiHints.tabs`), FM onset ≤ 8
  samples (MSFA LG_N=3), FM .syx params adopted into patch, noteMap from live graph, sampler load errors → UI.
  Per-engine note-on → first-sample latency all within one block (test `[latency]`).
- Signature presets: Doors + Pink Floyd (22), 80s pop + Rammstein (28), each reviewed.
- Phase 2 (merged, 166 Catch2 cases + 83 Vitest): `va` (reviewed twice, fixes merged), `fm` (MSFA, .syx),
  `organ`/`combo`/`rotary`, `epiano`/`tremolo`, `sampler` (sfizz 1.2.3, 16 sample presets), effects suite (9),
  `drums` + DrumSequencer + rhythm UI (12 patterns), plugin host (Faust JIT via faust.dll C API, C ABI DLLs,
  hot reload). Each branch had its own fresh-context review. Details: ARCHITECTURE §7, §8, §10; PLUGINS.md.
- Phase 1 core engine: CMake + pinned deps (JUCE 8.0.15 w/ bundled ASIO, nlohmann_json 3.12.0, IXWebSocket 11.4.6
  no TLS/zlib, Catch2 3.8.1; shared `.deps/` sources, per-build-dir dep builds, static MSVC runtime);
  core/ (ParamSpec/ParamSet, Module/Registry, MidiEvent, ChannelState, SpscQueue, VoiceAllocator, PatchModel,
  GraphBuilder w/ module reuse, RackGraph, GraphSwapper w/ crossfade/tail-out + render-once cache + retire queue,
  Telemetry, RtCheck, Engine); dsp/ (PolyBLEP, ADSR, smoothers, TPT SVF, soft clip, safety limiter, FTZ guard);
  modules `basic`, `gain`, `limiter`; preset/ (JSON, migrations stub, PresetStore); audio/ (AudioHost ASIO-first +
  settings.json, MidiHub, NullAudioDriver); platform/ MMCSS + power throttling; transport/ (Transport, Metronome,
  DrumSequencer stub); control/ (Session, ProtocolHandler, ControlServer WS 7341 + static HTTP 7340); render/
  (OfflineRenderer, MIDI file, WAV); tools ks-render, ks-bench; scripts/analyze_wav.py; docs/TESTING.md;
  2 factory presets. 39 Catch2 test cases pass (+ full render tier).
- Architecture rev 2 (after review #1). Protocol and preset format drafts.

- WP-D web UI for the platform (ARCHITECTURE §15 *Web UI*): `build:platform` (base `/keysynth/`) + runtime mode
  detection, `@mvv42/kit/platform` (useMe/apiJson/requestLogin/shell READY; no Mantine), engine discovery with LNA
  states + download/install card, automatic account link + mismatch confirmation + unlink hold, cloud card, sync
  pill, sample library drawer (download/cancel/delete, progress), auto-fetch toast + preset-row progress, update
  banner (GitHub latest), `.github/workflows/frontend.yml` (inline copy of infra deploy-static), dev platform stub;
  Vitest for discovery, link (against the mock engine), samples reducers, version compare.
- Phase 1 UI: generated module panels, rack/zones, FX chain, keyboard, audio dialog, mock engine. Reviewed, fixed.
- Sample libraries (core set ~3.5 GB, assets/samples.json) and Faust 2.88 (.tools/faust).

## In progress
- Shell tile: registry entry merged in portal-shell, but shell deploys fail on a flaky admin test; fix in
  portal-shell PR #11 (owner merges).

## Next
- Owner E2E on real hardware: download from the site → install → tray → «Подключить движок» (Chrome Local Network
  Access prompt) → automatic account link → preset with samples auto-downloads → play. Also inside the shell iframe
  (LNA permission may need an iframe `allow` attribute) and `keysynth://open` from the page.
- Plugin sync (deferred for security: needs local confirmation + hash pinning).
- Test on real hardware: ASIO panel button with UR22C, buffer change in the Yamaha driver app while playing
  (check `userdata/logs/audio.log`), play-feel per engine on the P-143.
- Ideas from preset work: wah, Binson Echorec-style drum echo, step sequencer/arpeggiator, pitch-bend envelope for
  leads, Leslie usable on combo organ presets, split zones shown on the on-screen keyboard, sampler panel shows
  loaded SFZ.
- Later: looper, recording, MIDI controller mapping, cross-instance sample cache, sandboxing paths inside SFZ files.

## Known issues / decisions log
- WP-D (web UI) decisions: only `@mvv42/kit/platform` is used (Mvv42Provider/AccountButton bring Mantine + global
  kit CSS that restyles the synth); own account button. `permissions.query` for LNA is treated as a hint (shown as
  denied only after a failed attempt). Owner check = our `GET /api/devices` row with the engine's `expiresAt`.
  Review items not fixed: account card's `plan()` reads `Date.now()`/socket status outside the store (may be stale
  until the next store change); «Портал» menu link is `/` (dev server root under `dev:platform`); `keysynth://open`
  is untested from the shell iframe (hidden there). Not yet verified in real Chrome against mvv42.ru (LNA prompt,
  iframe permission) — needs WP-E deploy.
- WP-C: the tray has no «Настройки звука…» item (spec §7): audio settings stay in the UI's audio dialog. Release
  CI (`release-windows.yml`) has not run yet: the code was only built with VS 18 / MSVC 19.50 locally; the
  runner picks VS 2022 or 2026 via `configure.ps1 -Generator auto`; Inno Setup from the image or choco 6.7.3.
  Review notes not fixed: a health probe finishing after shutdown drops its browser launch (harmless); the release
  exe still lists the CI's compile-time `KS_FAUST_DIR` as a last fallback (never exists on user machines).
- WP-B review items not fixed (accepted for v1): a `web` session's `load_preset` does not re-check paths *inside* the
  preset file (synced presets are server-controlled; still bounded by AppPaths roots); `set_patch` from `web` may
  hand a state blob to an already installed `plugin:` DLL (`load_state`, guarded); WS refusals are a 4003 close after
  the upgrade (ixwebsocket has no pre-upgrade hook). sfizz fails on non-ASCII `sample=` paths (index builder should
  warn). Replacing a cached file that a sampler streams from fails with `io_error` (delete is refused with `in_use`).
- Review #1 of Phase 1 (fresh-context subagent): fixed render-once cache sizing/pass-through, double processing of
  shared fx, metronome beat drop at block edges, stuck notes on zone-channel change / MIDI unplug, UNC path probing,
  node-id wrap, VoiceAllocator pedal/mono edge cases, set_patch reuse, transport validation, CC120/123 mapping.
  (noteMap-from-pending-graph issue: fixed in integration.)
- Deviations from ARCHITECTURE rev 2 are recorded inline there (RT checks ON in all configs; metronome + limiter in
  the Engine output stage; transition details; no module reuse across preset loads; Origin rules incl. port 7340).
- Phase 1 gaps: `transport.pattern/drums` -> `not_implemented` (DrumSequencer is a stub); RhythmNode is a
  placeholder; no SampleLibrary/PluginHost/SEH wrapper yet; sample-rate/buffer lists in `devices` only for the open
  device's type; MIDI events from devices are block-quantized (offset 0, by design); GC runs on the 30 Hz pump.
- Yamaha Steinberg USB ASIO (UR22C) only accepted its control-panel buffer (1024 @ 48 kHz, ~30 ms out latency
  reported) — set a small buffer in the driver panel for playing. Its callback thread is already MMCSS-registered
  by the driver (AvSetMmThreadCharacteristics -> ERROR_THREAD_ALREADY_IN_TASK, treated as success).
