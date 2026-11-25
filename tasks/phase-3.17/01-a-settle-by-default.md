# 01 — A settle by default

**Depends on:** — (phase 3.16's 02 and 03 are on `main`).
**Spec:** `tasks/phase-3.16/FOLLOWUPS.md` (*What the garden said: sessions 6 to 9*); `docs/noisescope.md` §5 (the `scope` command and its `ack`), §6 (*What a segment's width and a settle are for*), §7; `tools/noisescope/scope_plans.json`; `tasks/phase-3.16/reports/02.md` and `03.md`.
**Size:** a small session.
**Model:** claude-opus-5

## Goal

Every `scope` reads the first points of each segment wrong. Measured on the unit at Bellevue
(session 9, `scope 840 900 0 467 1000`, the neighbour at -33 dBm): the first six points of a
segment read -64.5, -74.5, -74.5, -78.5, -88.5 and -91.5 dBm in the frames that hold the
signal, and -98.5 where the floor is -104.5 in the frames that do not. With
`... max 32 500` they read the floor, -104, in both. With `... max 32 2000` the same. The
block follows the segments at every bandwidth from 467 down to 39 kHz.

Phase 3.16's task 02 built the settle and left the default alone until it was measured. It
is measured: **a `scope` that names no settle gets 500 µs**, and `0` is how none is asked
for.

## Deliverables

1. **The default.** A named constant for 500 µs beside `kScopeSettleMaxUs`
   (`src/roles/scanner/Sweep.h`). A `scope` line with no ninth number is planned, armed and
   swept with it; a line whose settle is `0` gets none, as today; the bound stays 20 000.
   Today the parser cannot tell *absent* from `0` (`ScopeArgs::settle_us`, `parseScope`):
   how it tells them is yours, and it is said in one place.
2. **Every plan.** More than one segment: the receiver stands on each segment's first point
   after its arm. One segment: before every sweep, as 02 built it for a settle that was
   asked for (`serviceScope`, the branch for a segment already armed).
3. **The plan's time and the port's interval count it**, as they already do for a settle
   that is asked for. The `ack`'s `sweep` stays true.
4. **The `ack` names the settle on every `scope` from now on** (`settle=500us`,
   `settle=0us`): a capture must say whether its first points stood. `segw=` stays as 02 has
   it, named only when it is not 32. The four words stay as 03 has them.
5. **The plan table.** `tools/noisescope/scope_plans.json` is read by this firmware's tests
   and by `tools/noisescope/test_echoes.py`: a row that carries a planned time or a settle
   changes with the default, and one row is added for the same line with `0`. Both tests
   green on the same file.
6. **The garden's lists.** A line of `tools/noisescope/lists/garden-*.txt` that is the
   reference *without* a settle (`garden-d`'s first line and its fifth) says so with
   `max 32 0`; `hunt.py --sessions` still takes every list (40 characters, 384 bytes).
   `garden-d` is at 384 of 384 bytes today: make the room with the lines' own means (the
   points left to the bandwidth, a step dropped) and say what moved.
7. **Tests:** the default taken with no number, with a width and no settle, and with the
   words; `0` taken as none; 20 001 refused; the arm's calls in order with the default, for
   a first segment, a later one and a reload; a one-segment plan stands before every sweep;
   the planned time for six segments at 467 kHz with and without; the `ack`'s bytes for
   both. Tests that held *a line with six numbers arms as it did* are brought up to date
   and named in the report, each with what it held before.
8. **The words.** `docs/noisescope.md` §5: the default in the command's row and the `ack`'s
   paragraph; §6: the paragraph on the settle says what was measured and where (the numbers
   above; four sessions, one unit) and no longer that the default is undecided; §7: a short
   paragraph, *what a wide picture cannot tell*: around a strong signal the floor reads
   about 35 dB under it, out to some 10 MHz either side at 467 and 373.6 kHz and about
   4.4 MHz at 117 kHz and below; and at 234 kHz and wider a strong signal on 869.618 MHz
   shows again at 816.0, 845.0, 887.7 and 912.0 MHz. Measured, cause not known, the
   follow-ups named. A line in `release-notes/unreleased/`: a `scope` no longer reads the
   first points of its segments high.

## Acceptance criteria

- [ ] A `scope` with no settle named is armed with 500 µs on every segment, and one with `0` as on `main` today (the tests' names).
- [ ] A one-segment plan stands on its first point before every sweep by default (the test's name).
- [ ] The planned time and the port's interval count the default (the test's name, with the numbers for six segments at 467 kHz).
- [ ] The `ack` names the settle on every `scope` (the two lines printed in the report).
- [ ] `scope_plans.json` holds for the firmware's test and for `test_echoes.py`; the garden's lists are taken by `hunt.py --sessions`.
- [ ] Nothing else of a `scope` moves: the plan's points and segments, the words, `us`, `usx`, `dev` (the tests that hold them, unedited).
- [ ] `tools\verify.ps1` green. CI green.

## What only the unit can show

One session after the release, two lines: `scope 840 900 0 467 1000` and
`scope 840 900 0 467 1000 max 32 0`. The first six points of each segment at the floor in
the first and standing up in the second. Thomas's or mine, from meshmaster.dk.

## Out of scope

A segment's width by default. The bandwidth of the wide picture and meshmaster.dk's
proposal (after the next sessions). What the far echoes are. The plugin's words. A finer
clock for `us`. A release.
