# Status

Update when a chunk of work lands. Newest first inside each section.

## Done
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

- Phase 1 UI: generated module panels, rack/zones, FX chain, keyboard, audio dialog, mock engine. Reviewed, fixed.
- Sample libraries (core set ~3.5 GB, assets/samples.json) and Faust 2.88 (.tools/faust).

## In progress (Phase 2, parallel worktree branches)
- `va`, `fm`, `organ`+`combo`+`rotary`, `epiano`+`tremolo`, `sampler` (sfizz), effects suite,
  `drums`+DrumSequencer+rhythm UI, plugin host (Faust JIT + C ABI DLL, hot reload).

## Next
- Merge phase 2 branches; integration review.
- UI against the real engine; ASIO control-panel button (see Yamaha buffer note below).
- Phase 4: signature presets (Doors, 80s pop, Rammstein, Pink Floyd) with FX, tuning by render analysis.
- Later: looper, recording, MIDI controller mapping.

## Known issues / decisions log
- Review #1 of Phase 1 (fresh-context subagent): fixed render-once cache sizing/pass-through, double processing of
  shared fx, metronome beat drop at block edges, stuck notes on zone-channel change / MIDI unplug, UNC path probing,
  node-id wrap, VoiceAllocator pedal/mono edge cases, set_patch reuse, transport validation, CC120/123 mapping.
  Open (low): GraphBuilder copies noteMap from the latest (possibly never-live pending) graph.
- Deviations from ARCHITECTURE rev 2 are recorded inline there (RT checks ON in all configs; metronome + limiter in
  the Engine output stage; transition details; no module reuse across preset loads; Origin rules incl. port 7340).
- Phase 1 gaps: `transport.pattern/drums` -> `not_implemented` (DrumSequencer is a stub); RhythmNode is a
  placeholder; no SampleLibrary/PluginHost/SEH wrapper yet; sample-rate/buffer lists in `devices` only for the open
  device's type; MIDI events from devices are block-quantized (offset 0, by design); GC runs on the 30 Hz pump.
- Yamaha Steinberg USB ASIO (UR22C) only accepted its control-panel buffer (1024 @ 48 kHz, ~30 ms out latency
  reported) — set a small buffer in the driver panel for playing. Its callback thread is already MMCSS-registered
  by the driver (AvSetMmThreadCharacteristics -> ERROR_THREAD_ALREADY_IN_TASK, treated as success).
