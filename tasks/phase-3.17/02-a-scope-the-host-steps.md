# 02 — A scope the host steps

**Depends on:** 01 (the default settle, and the line it adds to).
**Spec:** `tasks/phase-3.17/FOLLOWUPS.md` (*Sessions 10 and 11*); `src/roles/scanner/Radio.cpp` (`serviceScope` ~L501–560; `ChipArmRadio` ~L470–496: `setFrequency(mhz, true)` in continuous receive and `settle`; `retuneRx` and `sampleWindow` ~L238–262, what the host-stepped `sweep` does); `src/roles/scanner/Sweep.h` and `Sweep.cpp` (`planScope`, `armScopeSegment`, `settleScopeSegment`, the words of phase 3.16's 03); `src/roles/scanner/vendor/sx126x_spectrum/spectrum.asm` (`retune`: the plugin writes the frequency registers itself and reads L updates later); `docs/noisescope.md` §5, §6, §7, §8; `config.h` (`LOOP_WDT_TIMEOUT_MS`).
**Size:** one session.
**Model:** claude-opus-5

## Goal

The far echoes of a wide `scope` are the sweep plugin's and not the chip's (sessions 10 and
11 on the unit at Bellevue, 25 November):

- The receiver parked on 912.0 MHz, and a host-stepped `sweep` over 905 to 919 and 811 to
  821 MHz at the same 467 kHz, never showed the neighbour's signal. The plugin's `scope` of
  the same place, in the same session, showed it at -61 dBm.
- An echo is gone when a **segment begins at its place and stands the settle**: with a
  start at 816.25 and 912.25 MHz the blocks at 816 and 912 read the floor (-104 and
  -106 dBm, from -64.5 and -61); with a start at 845.25 the block at 845 does. Mid-segment
  they are there every time.
- So the plugin, which writes the frequency registers itself and reads within some 10 µs,
  reads through a moment the synthesizer needs at certain frequencies (30 to 40 µs), and the
  chip's own way of tuning with a wait does not.

Two of those frequencies are round (816.0 and 912.0 MHz are 25.5 and 28.5 times the 32 MHz
crystal, and 864.0, 27 times, shows too) and two fit no rule (845.1 and 887.6 or so): a
list of places to cut at cannot be written down. **What can be built is a `scope` in which
every point is tuned the chip's own way and stood on before it is read.** At 467 kHz that
costs little: six segments of the plugin take about 105 ms to arm for 1.3 ms of sweeping.

The plugin stays what `scope` does. This task adds the other way as a word, and the unit
says which is better where.

## Deliverables

1. **A word, `stepped`,** taken out of the line wherever it stands after the two ends, as
   03's four words are, and holding for that one `scope`. The `ack` names it
   (`mode=stepped`). `scope 780 960 0 467 1000 stepped` is 32 characters; a test holds the
   examples to a session's 40.
2. **What a stepped point is.** The receiver stays in continuous receive at the plan's
   bandwidth. For each point: the frequency is set with the chip's own command through the
   arm's seam (`ScopeArmRadio::setFrequency`, the calibration skipped inside a range that is
   calibrated; the image calibrated for each 32 MHz range exactly as an arm does it today);
   the receiver **stands** the settle that is in force (01's default, or the line's), and
   never less than the time the plugin's own latency table gives a reading to be of the new
   frequency (L + 2 updates of 1.6 ms / RBW); then the level is read once. Waits are made
   with the delay the settle uses, not by reading `micros()`: on this board it steps by
   977 µs (`tasks/phase-3.16/FOLLOWUPS.md`).
3. **The plan** is the same points, step and bandwidth as the plugin's; a segment is a
   range of one calibration and nothing else. The planned time is the points times what a
   point costs, said in the `ack`'s `sweep`, and the port's interval follows it.
4. **The loop is not held.** A sweep is done in slices, a pass of `loop()` each, so that a
   `stop`, the heartbeat and both watchdogs are served as during a plugin scope: say the
   slice and why. A `stop` in the middle of a sweep ends the scope and sends no half frame.
5. **A frame is a frame.** The same line, the same members; `agg` works over stepped sweeps
   as over plugin ones; `us` and `usx` are a whole stepped sweep; `dev` is read where a
   range is calibrated, as today.
6. **Host tests:** the word in every position, with 03's words and with the numbers; the
   calls for three points in order (frequency, settle, read) and a calibration at each
   32 MHz range; the stand never under the latency's time at 4.8, 58.6 and 467 kHz; the
   planned time for 386 points at 467 kHz and 250 at 58.6; a `stop` mid-sweep; a plugin
   `scope` after a stepped one is the plugin's (the test that holds *the radio is what it
   was*, extended).
7. **The garden's list**, `tools/noisescope/lists/garden-e.txt`, taken by
   `hunt.py --sessions`: the wide span the plugin's way and stepped, twice each, then
   13 MHz at 58.6 kHz both ways:
   `scope 800 960 320 467 1000`, `… stepped`, the first again, `… stepped` again,
   `scope 863 876 0 58.6 1000`, `… stepped`. `echoes.py --steps` reads `mode=` from the
   `ack` and its pair line for *the mode* says, for each echo of one, whether the other has
   it.
8. **The words.** `docs/noisescope.md` §5 (the word, the `ack`), §6 (why there are two ways
   to sweep: what the plugin is fast at and what it reads through; the sessions' numbers,
   in our words), §8. A line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] `scope … stepped` tunes every point with the chip's own command, stands the settle and never less than the latency's time, and reads once (the tests' names; the order printed for three points).
- [ ] The planned time and the port's interval are a stepped sweep's (the numbers for 386 points at 467 kHz in the report, beside the plugin's).
- [ ] The loop is served during a stepped sweep and a `stop` ends it cleanly (the tests' names).
- [ ] A `scope` without the word is 01's, to the byte of its `ack` and its arm (the tests that hold it, unedited).
- [ ] `garden-e` is taken by `hunt.py --sessions`, and `echoes.py --steps` pairs a stepped scope with a plugin one.
- [ ] `tools\verify.ps1` green. CI green.

## What only the unit can show

One session of `garden-e` after the release: in the frames that hold the neighbour's
signal, the plugin's steps show 816, 845, 888 and 912 MHz and the stepped ones do not; and
what a stepped sweep of the wide span costs in sweeps a second (the plugin makes 11 with
five segments). Mine to start and read, from meshmaster.dk.

## Out of scope

Which way is the default, for any span. meshmaster.dk's proposal. Cutting segments at known
places (816.0 and 912.0 could be; 845.1 and 887.6 fit no rule, and a list would be one
chip's). The plugin's words. A release.
