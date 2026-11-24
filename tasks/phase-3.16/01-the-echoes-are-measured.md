# 01 — The echoes are measured

**Depends on:** —
**Spec:** `tasks/phase-3.15/FOLLOWUPS.md` (*The echoes in the wide scope*) and this pile's `README.md` (*What the two captures say*); `docs/noisescope.md` §5 (the `scope` command, its `ack`, the `scope` line), §6 (*The sweep plugin*, *Why a wide span is several sweeps*) and §8; `src/roles/scanner/Sweep.h` (the scope's constants ~L256–324, `ScopeSegment` ~L354, `ScopePlan` ~L400, `planScope` ~L418, `scopeSegment` ~L421, `armScopeSegment` ~L486) and `Sweep.cpp` (`armScopeSegment` ~L359, `scopeSegmentMs` ~L407, `scopeSweepMs` ~L413, `planScope` ~L425, `scopeSegment` ~L487); `src/roles/scanner/Radio.cpp` (`beginPlugin` ~L421, `ChipArmRadio` ~L443, `emitScopeFrame` ~L480, `serviceScope` ~L501, `startScope` ~L665); `src/roles/scanner/vendor/sx126x_spectrum/spectrum.asm` (whole, 157 lines) and `SX126xSpectrumScanner.h` (`latencyFor` ~L78, `setSweep` ~L130–162, `sweep` ~L167); `tools/bench/` (`bench.py` `console --scanner "<line>" --collect SECONDS` ~L309–327, `console.py` `collect` ~L307, `fakes.py`), `tools/noisescope/waterfall_plot.py` (`scope_levels_to_dbm` ~L73); `test/test_scanner/test_scanner.cpp`; `docs/bench.md` §0a and §9 (*The fast sweep*: the image check, never walked); `tools/verify.ps1` (step 5 runs the tools' own tests); the SX1261/2 datasheet (the synthesizer, the receiver's gain control, `GetRssiInst`)
**Size:** one session.
**Model:** claude-opus-5

## Goal

A wide `scope` shows signals that are not there. On 23 November (Bellevue, meshmaster.dk
`/noisescope/3`) the picture of `scope 779.618 959.618 0 467 1000` had bright blocks at 816,
840 to 847, 888 to 893, 900, 912 and 930 MHz in every frame in which a neighbour 200 m away
transmitted on 869.618 MHz at -32 dBm, and faint lines at the same places always. The radio
sits behind a cavity filter that passes 863 to 870 MHz.

The README has what two captures say: **the blocks begin at the first point of a segment of
the sweep and again about every 5.6 MHz after it**, in a capture at 467 kHz and in one at
58.6 kHz, and each fades over some ten readings in the sweep's direction. That is a reading of
two captures from one unit, not a measurement.

After this task the echoes can be counted by a tool, the plugin's part in them is read out of
its own source, and, where the bench has a carrier on it, the questions below are answered
with numbers from the board. **Nothing of what `scope` does changes in this task.**

## Deliverables

1. **A tool that finds them: `tools/noisescope/echoes.py`.** It reads a capture (NDJSON as
   the scanner says it) and, for every `scope` in it:
   - lays the segments out from the `ack` and the frames, by the same arithmetic as
     `planScope` and `scopeSegment`. That arithmetic exists once in C++; the tool's copy is
     held to it by a table of plans both tests read (one file, say where);
   - takes the carrier (`--carrier <MHz>`, or the strongest point) and the frames that hold
     it (`--lit <dBm>`, default -70; with a steady carrier that is every frame);
   - finds every run of points that stands `--over <dB>` (default 6) above the floor in those
     frames and is not the carrier's own run;
   - says for each: its first frequency, its segment, **its distance from that segment's
     first point in MHz, in points and in microseconds** (a point is one RSSI update,
     1.6 ms ÷ RBW in kHz), its strongest level, that level below the carrier's, and its
     length in the same three units;
   - says in one line whether the distances repeat, in which of the three units they repeat
     best, the interval and the worst miss;
   - prints a table, and the same as JSON with `--json`. Exit 0 with or without echoes; a
     capture it cannot read is the error.
2. **Its tests**, run where `tools/bench`'s are: captures made by the test itself with echoes
   planted three ways (every 5.6 MHz from a segment's first point; every 12 points; every
   41 µs), at two bandwidths and two steps each, and the tool names the way they were
   planted. A capture with a carrier and no echoes says so. Lines that are not `scope` lines,
   a frame with `255` in it and a `scope` of one segment are read without complaint.
3. **The captures of 23 November, when they are there.** Thomas may have saved the lines of
   `/noisescope/3` as `tools/noisescope/captures/bellevue-2026-11-23.ndjson`. If the file is
   there: a test holds the tool to the README's table for its second and third `scope`, and
   the report prints the tool's own table for both. If not: say so; nothing fails.
4. **The plugin, read.** `spectrum.asm` whole and `setSweep`. In the report, in sentences a
   person who has not read it can follow:
   - what one point costs in the plugin's own instructions, and what that is against one
     update at 4.8, 58.6 and 467 kHz. At 467 kHz `setSweep` works out a negative `delay` and
     sends 0, and a `wait` of 17 reads: say what the poll loop then does;
   - what `wait_update` does when a reading does not change, and what that does to the
     latency it counts in updates;
   - what the chip is told at a retune (four register writes, the last commits), and what the
     arm leaves the receiver doing before the first point (`setSweep` puts it in receive at
     the segment's **centre**, and point 0 is half a segment away);
   - which of these could put an echo at a segment's first point, which could repeat every
     so many MHz, points or microseconds, and which measurement of deliverable 6 would tell.
   Guesses are marked as guesses.
5. **A script that asks the board: `tools/noisescope/hunt.py`.** Through `tools/bench`'s
   console and nothing else (no port opened by hand; exit 3 with no bench, as the tool
   does). It runs a list of `scope` lines, each for `--seconds` (default 10) with interval 0
   and `last`, keeps each capture under `out/bench/echoes/<label>/`, runs deliverable 1 on
   each and writes one table for the run. `--label`, `--carrier`, `--list <file>` for another
   list. Tested against `tools/bench/fakes.py`'s scanner, extended as it needs.
6. **The questions, asked of the board** when `python tools/bench/bench.py status` finds one
   (the README says what Thomas has put on its antenna port, and that **this task flashes the
   scanner and nothing else and does not run `bench restore`**). Build the scanner, flash it,
   `info`, then the list. Each answer is a table in the report, made by the tool:
   - **Where they are**, as the unit took it: `scope <c-90> <c+90> 0 467`, and `scope` of
     13 MHz at 58.6 kHz around the carrier.
   - **Do they move with the segments?** The same 180 MHz begun 7 MHz lower; and spans of
     60, 31 and 20 MHz around the carrier, which are two, one and one segment.
   - **Is the interval MHz, points or time?** The 180 MHz again with the step halved (772
     points at 467 kHz), and at 234.3, 117.3 and 58.6 kHz.
   - **How long is a tail, in what unit?** From the same captures.
   - **Does the carrier land on its own point** at each bandwidth (the latency of 2, 3 or 4)?
   - **What a sweep costs**: `sps` of each line.
   - **With boosted gain off** (`boost off`), the first line again.
   If `c` is not within 1 MHz of 869.618, or the carrier is below -60 dBm at the radio, say
   so and go on: the tables say what they were made with.
7. **The captures behind the tables are kept**: copied to
   `tools/noisescope/captures/p3.16-01/`, as collected, with a line each in a `README.md`
   there (the line sent, the carrier, the date). Not the whole of `out/bench`.
8. **The words.** `docs/noisescope.md` §7 gains what a reader of a wide `scope` must know
   today: where its echoes fall and that a strong signal in the passband makes them, with
   the numbers this task measured or, with no bench, the two captures' and the word *read,
   not measured*. `docs/bench.md` §9: the two tools, and the image check's line says which of
   its questions this task answered. A line in `release-notes/unreleased/` only if a person
   will notice something, which the README says they will not.

## Acceptance criteria

- [ ] `echoes.py` names the planted rule in every one of its synthetic captures, at both bandwidths and both steps (the tests' names).
- [ ] The tool's segments are `planScope`'s for every plan of the shared table (the file, the two tests).
- [ ] With the capture of 23 November present, the tool's table for its third `scope` has the README's rows; absent, the report says so.
- [ ] The report's section on the plugin answers deliverable 4's four points, each with the lines of `spectrum.asm` or `setSweep` it rests on.
- [ ] `hunt.py` runs its list against the fake scanner and exits 3 with no bench (the tests' names).
- [ ] With a bench: every question of deliverable 6 has its table, the captures are in `tools/noisescope/captures/p3.16-01/`, and the board is left running the scanner this task built. Without: *Not verified here*, and the list a person runs is one command in the report.
- [ ] `scope` is what it was: the scanner's image differs from `main`'s by nothing but its version, or each difference is named.
- [ ] `tools\verify.ps1` green. CI green.

## What only a board can show

All of deliverable 6. And beyond this task: a second carrier frequency (865.0 MHz), the
carrier 10 and 20 dB weaker, and the dummy load alone, each one run of `hunt.py --label`.
Thomas's; I read the tables.

## Out of scope

Any change to what `scope` does, to the plugin's words, to `kScopeSegmentMhz` or to the
latency: task 02 gives the knobs, and nothing is mended by a guess. The Walter. meshmaster.dk.
A release.
