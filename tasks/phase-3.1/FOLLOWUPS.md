# Phase 3.1 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

## Review of 01 (14 October)

01 landed in one session of 56 minutes, CI green in the first round (PR #20,
merge `ee00328`). The plugin is vendored with its source, its licence and a
header naming repository, branch and commit, and a native test pins its 167
words; the plan, the frames and the sink rule are host-portable and tested
(23 new cases, 359 in all). Three of the session's own decisions are better
than the task's text, and each is in the report: the task's two defaults
were circular (an explicit `rbw` now sets the points; with neither, 250
points, one plugin call, which is what the bench line's *tens of sweeps a
second* needs); a segment is calibrated for the range it **measures**, not
its nominal bins (a test over every legal point count caught 50 MHz
calibrations against a 32 MHz bound); and the segment count is not a
refusal, so lowering `kScopeSegmentMhz` at the bench can never start
refusing wide scopes. The plotter's spectrum-over-waterfall, `--range`,
`--mark`, `--baseline` and the grey band for `cov < 1` were checked on
synthetic logs only.

### Open

- [ ] **The bench walk, `docs/bench.md` §9 *The fast sweep*** - six lines,
  and the first gates the rest: `info`'s `rom` on a real RAK4630 must read
  `SX1261 V2D 2D02`, or the plugin is not for this chip and `scope`
  refuses. Then the speed over USB, the dummy-load baseline over 150–960,
  the image check with the tinySA Ultra as a generator (the one line that
  may lower `kScopeSegmentMhz`), the site through the cavity filter, and
  ten minutes of `scope` from the Walter UART with no watchdog reset.
  Thomas's, with a dummy load on the RAK's antenna port for the baseline.
- [ ] **The stale remote branch** `origin/task/p3.1-01-the-fast-sweep`:
  the driver could not delete it (`logs/run-all.log`); delete it by hand.
- [x] *(written 18 October: `../phase-3/FOLLOWUPS.md`)* **Phase 3's own review** (`../phase-3/FOLLOWUPS.md`) is still
  *Nothing yet*; 3.1 ran without it. Nothing in 3.1 depends on it, but the
  roles, the descriptor and the first-boot profile have had no second
  reading.

## From a bench walk of section 9 (2026-10-22)

- [ ] **[01] `docs/bench.md` 9.9 differs from the board** *(→ p3.5-02 takes it)* (a bench walk of 2026-10-22 at `9fb721b`). `scope 150 960 2048` on the dummy load, the NDJSON kept and the picture saved
  the baseline: `scope 150 960 2048` on the dummy load, kept: saw `{"cmd": "scope", "msg": "n=2048 step=395.5078k...", "ok": true, "t": "ack"} / 0 frame(s) in 20 s -> out/bench/baseline-150-960-20261022-174804-9fb721b.ndjson / err: scope setup failed (-707) at 212.6880`, expected `None`.
  The walk's result file under `out/bench/` has the transcript.

  **What p3.4-02 measured around it**, with the scanner on the first real board (`rom`
  `SX1261 V2D 2D02`, the dummy load on the antenna port, `bench console --scanner`): the span's
  **segment count** is what decides it, not the frequency and not the rbw.

  | sent | ack | what came back |
  |---|---|---|
  | `scope 863 870` | n=250 seg=1 rbw=39.0k | frames, `sps` up to 41.6 a second |
  | `scope 150 200` | n=250 seg=2 rbw=234.3k | frames, `sps` 175 |
  | `scope 200 250` | n=250 seg=2 rbw=234.3k | frames, `sps` 172 |
  | `scope 240 300` | n=250 seg=2 rbw=312.0k | frames, `sps` 172 |
  | `scope 150 213` | n=250 seg=2 rbw=312.0k | frames, `sps` 178 |
  | `scope 150 250` | n=250 seg=4 rbw=467.0k | `err: scope setup failed (-707) at 200.6000` |
  | `scope 150 250 500` | n=500 seg=4 rbw=234.3k | `err: scope setup failed (-707) at 200.1000` |
  | `scope 150 960 1024` | n=1024 seg=26 rbw=467.0k | `err: scope setup failed (-707) at 213.6768` |
  | `scope 150 960 2048` | n=2048 seg=26 rbw=467.0k | `err: scope setup failed (-707) at 212.6880` |
  | `scope 300 400` | n=250 seg=4 rbw=467.0k | no frame and no `err` in 15 s |
  | `scope 500 600` | n=250 seg=4 rbw=467.0k | no frame and no `err` in 15 s |

  So: **one or two segments stream; four or more either fail at the third segment with
  `RADIOLIB_ERR_SPI_CMD_FAILED` (-707, `TypeDef.h:440`) or go silent.** `-707` comes out of
  `armSegment` (`src/roles/scanner/Radio.cpp:357-369`: `standby()`, `setFrequency(centre)`,
  `calibrateImageRejection(lo, hi)`, `setSweep(...)`) and the walk cannot tell which of the four
  calls returned it — the next session on this should log the call as well as the frequency. Two
  of the page's own lines rest on this: *The baseline* (`scope 150 960 2048`, seg 26) and *The
  site through the cavity filter* (`scope 791 960`, seg 6). `scope 863 870`, the line the fast
  sweep exists for, is seg 1 and works.
