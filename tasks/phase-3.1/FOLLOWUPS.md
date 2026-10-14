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
- [ ] **Phase 3's own review** (`../phase-3/FOLLOWUPS.md`) is still
  *Nothing yet*; 3.1 ran without it. Nothing in 3.1 depends on it, but the
  roles, the descriptor and the first-boot profile have had no second
  reading.
