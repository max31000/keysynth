# Status

Update when a chunk of work lands. Newest first inside each section.

## Done
- Architecture rev 2 (after review #1). Protocol and preset format drafts.

## In progress
- Phase 1: core engine (core/, audio host, MIDI, control server, offline renderer, tests).
- Phase 1: UI against docs/PROTOCOL.md — ui/ built with mock engine (`npm run mock`); pending: run against the real engine.

## Next
- Phase 2 (parallel): engines `va`, `fm`, `organ`+`combo`, `epiano`, `sampler`, `drums`; effects suite.
- Phase 3: transport (metronome, drum sequencer), plugin ABI + Faust hot reload, sample library fetch.
- Phase 4: factory presets for reference sounds (Doors, 80s pop, Rammstein, Pink Floyd), tuning by render analysis.

## Known issues / decisions log
- (none yet)
