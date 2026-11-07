# Phase 3.9 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–03 goes below them when it
is written.

## Found while doing 01 (7 November)

- [ ] **[03] The notes of v0.1.2 must say what 01 found.** The published v0.1.1 notes say the radio came
  back *with its key*. v0.1.2's notes say the opposite in one paragraph: the key was lost because the
  Adafruit core's `InternalFS.begin()` formats a volume it cannot mount, and from v0.1.2 the radio
  refuses instead (`IDENTITY LOST`, `erase` + `reboot`); `docs/flashing.md` §3a is the page. And that
  a Walter sees it as `IDENTITY` answered `ERR_STATE`, `caps 0x21`, attention 6/1.
- [ ] **[Thomas] The published v0.1.1 release body still says *with its key*.** Edit the GitHub release
  body (one sentence, with the one about the Walter already on the list in `../phase-3.8/FOLLOWUPS.md`),
  or leave it and let v0.1.2's notes correct it. `release-notes/v0.1.1.md` in the repository was left
  as published, on purpose: it is the record of what was said.
- [ ] **[reviewer] The clock keeper writes the file system twice in the first seconds of every boot.**
  `ClockKeeper::begin` restores `saved + 3600` with `file_epoch_ = saved`, so the first `loop()` a
  second later finds `t > file_epoch_ + interval` and writes; then the Walter's `TIME_SET` brings the
  clock *down* from the margin and `saveBoth(..., force_file=true)` writes again. Each is one erase and
  program of the page that holds the superblock and the root directory, in the seconds a second reset
  is most likely (a power-up glitch, a double-tap, a brownout). 01 did not change it: the first write
  is what keeps a quick reset from restoring a time behind an advert already sent (tests in
  `test/test_clock`), and whether to defer it until the advert hold ends, or to move the file to a
  page of its own, is a design question for the clock's owner. The report's *Decisions* has the
  reading.
- [ ] **[Walter; written in the report in its FOLLOWUPS form] The Walter's pages say the key was kept.**
  Its `tasks/phase-8/README.md` (*recovered … its key kept*), `01-the-flasher-lets-the-idle-task-run.md`
  (*its key survived*) and the v0.1.1 notes. And `agent id` should say so when `IDENTITY` answers
  `ERR_STATE`, rather than printing nothing.

