# Phase 1 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

Nothing yet.

## From a bench walk of section 3 (2026-10-22)

- [ ] **[04] `docs/bench.md` 3.9 differs from the board** (a bench walk of 2026-10-22 at `b391890`, this branch's code). `clock sync` at the console, and whether it moves the state the same way
  `clock sync` moves the state the same way: saw `ERR: clock cannot go backwards / (`clock.state` is now set)`, expected `OK - clock set: <HH:MM - D/M/YYYY UTC>, and `clock.state` set`.
  The walk's result file under `out/bench/` has the transcript.
