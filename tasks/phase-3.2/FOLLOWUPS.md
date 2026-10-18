# Phase 3.2 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

## Review of 01 (18 October)

01 landed in one session, CI green in the first round (PR #22, merge `ac7f2d4`). The two tests
now build what they expect from the defines and still fail a build whose defines did not arrive
(shown both ways); the rehearsal bump from `repeater-v1.17.0` was green on its own pull request
(#21) with no session involved, and the phase-0 item is ticked. The session pushed two reviewer
commits to `main` first rather than carry them in its pull request, which was right.

- [ ] **[3.3] `upstream.yml`'s pull-request body says *seven places*; the page and the template
  say eight** (01, *Deviations*). That body is what a person reads before merging a green bump.
- [ ] **[3.3] `sensor list`'s paging arithmetic is tested against a fixed-length version string**
  (01, *Not verified here*): a longer upstream tag changes what the device prints and not what
  `test_settings` asserts. Build the fake's version string from the real format at the longest
  tag the format allows, or assert the bound the arithmetic depends on.
- [ ] **[Thomas]** Delete `origin/task/p3.2-01-a-bump-can-be-green` (still listed here) and, if it
  is still on GitHub, `origin/task/p3.1-01-the-fast-sweep`.
- [ ] **[Thomas]** The first real bump is `docs/bench.md`'s p0-03 line: on the Monday after the
  next `repeater-v*` tag.
