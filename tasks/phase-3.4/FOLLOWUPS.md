# Phase 3.4 — follow-ups

Review findings, tagged with the task that must pick them up (`[02]`), `[Thomas]` or `[reviewer]`.
Only the reviewer ticks an item.

## From p3.4-02, the first board (22 October 2026)

Two warts of `tools/bench` that the first board showed and that 02 did not change, because each
is a change to what a command *means* rather than a fix to what it does:

- [ ] **[reviewer] A board that arrives in its bootloader leaves a run nothing can restore.**
  `bench flash` on a radio with a base and no application records `role: None, how: bootloader`
  (`runs.record_board`), which is honest - nothing of what it ran can be read - but then
  `bench restore` says `THE RUN RECORDED NO RECOGNISABLE ROLE ON THE BOARD` and the walk of
  section 8 ends in a *differs* instead of a restore. 02 got past it with a second
  `bench record --image out/build/repeater_rak4630_single/firmware.zip` after the first flash,
  which is the state to put back for a first unit. Either the message should name that command,
  or `bench flash` on a bootloader-only board should record the image it is about to write as the
  run's `found_image`. It is the normal case for every new unit, so it is worth one of the two.
- [ ] **[reviewer] `--tick` says *not found in docs/bench.md* for a line that is already
  ticked.** A second walk of a section prints `not found in docs/bench.md, so not ticked: 9.2,
  9.4, 9.7, 9.8` when those lines are ticked from an earlier run - `tick_page` looks for
  `- [ ] ` and an already-ticked bullet is `- [x] `. Nothing is wrong and nothing is lost (the
  report of p3.3-03's *Decisions* 1 says ticking twice changes nothing), but the wording says
  the page lost a line it did not lose. *already ticked* would be the true word.
