# 01 — A histogram that adds up

**Depends on:** —
**Spec:** `tasks/phase-3.14/FOLLOWUPS.md` (*The scanner's histogram, as session 1 read it*); `src/roles/scanner/Radio.cpp` (`serviceHist` ~L301–344, `startHist` ~L606–621), `src/roles/scanner/Sweep.h` (`kHistBins`, `kHistSamplesMin`; the seam `ScopeArmRadio` ~L281–300 and `armScopeSegment` ~L344: how a chip sequence is held by a host test here) and `Sweep.cpp` (`clampHistSamples` ~L52), `src/roles/scanner/Lines.cpp` (`hist` ~L214–226), `src/roles/scanner/config.h` (`HIST_SAMPLES_DEFAULT` ~L95); RadioLib 7.8.0 as `.pio/libdeps/scanner_rak4630_single/RadioLib` has it: `src/modules/SX126x/SX126x.cpp` (`spectralScanStart` ~L1191, `spectralScanAbort` ~L1208, `spectralScanGetStatus` ~L1212, `spectralScanGetResult` ~L1220), `SX126x_registers.h` (the status register and its four values ~L59, ~L128–132) and `examples/SX126x/SX126x_Spectrum_Scan_Frequency`; `test/test_scanner/test_scanner.cpp` (`FakeArmRadio` ~L1380, the `hist` line ~L575); `docs/noisescope.md` §5 (*Commands* ~L211, ~L264; *Messages* ~L359–367) and §7; `docs/bench.md` §9
**Size:** one session.
**Model:** claude-opus-5

## Goal

`hist f0 f1 step` measures one frequency after another: per frequency the SX1262's spectral
scan takes `n` RSSI samples 8.2 µs apart and counts them into 33 bins of 4 dB. A line's bins
therefore add up to its `n`. On 21 November they did not (Bellevue, scanner 0.1.6+fcf52d5,
`hist 869.418 869.818 25`, `n` 2 048):

| `seq` | MHz | bins add up to | of which one bin holds |
|---|---|---|---|
| 245 | 869.418 | 2 048 | — |
| 246 | 869.443 | 15 | — |
| 247 | 869.468 | 15 | — |
| 248 | 869.493 | 8 | — |
| 249 | 869.518 | 4 140 | 4 096 (`0x1000`) |
| 250 | 869.543 | 32 | — |
| 251 | 869.568 | 1 807 | 1 792 (`0x0700`) |

The lines' own `ms` are 22 ms apart. A scan of 2 048 samples is 16.8 ms, and a line of about
170 bytes is 14.8 ms of a 115 200-baud port: the scans were not waited for. Without the two
bins that are a high byte and nothing else, every line after the first holds 8 to 44 samples: a
result read within 0.4 ms of its scan's start.

`serviceHist` starts a scan and asks `spectralScanGetStatus()` at once. RadioLib's start writes
the RSSI window, `SetRx` and the scan's parameters; nothing in it touches the status register,
and its abort writes the window register only. If the register still reads *completed* from the
frequency before, the first poll takes that for this scan and the result is read while the scan
has barely begun: a few samples, and now and then a counter caught half-written. That is a
reading of the numbers and of RadioLib's source, not a thing seen on a chip, and RadioLib's own
example polls the same way. No session has a chip.

After this task a `hist` line is the histogram of the scan that was started for it, the
firmware proves that to itself with a sum before it says the line, and a result that cannot be
proved is never sent as a histogram.

## Deliverables

1. **A result belongs to the scan started for it.** Whatever the status register reads when a
   scan is started, only this scan's completion is taken. How is yours; these two hold either
   way and both are wanted:
   - a scan of `n` samples cannot have finished before `n` × 8.2 µs have passed since it was
     started, so a *completed* read sooner is not this scan's and is not acted on;
   - the result is read once, after the completion, never while the scan runs.

   If the register can be put back to *none* before a start, or seen to pass through *ongoing*,
   say so in the report and use it as well; do not rest on it alone.
2. **The sum is the proof.** The 33 bins of a result are added before anything is said. They
   add up to `n` or the result is not a histogram. A result that does not add up is measured
   again at the same frequency: three scans at most. If the third does not add up either, one
   `error` line with the frequency, the sum and `n`, and the pass ends as the function's other
   failures end it. **No `hist` line leaves the scanner whose bins do not add up to its `n`.**
3. **The sequence behind a seam.** The steps for one frequency (standby, frequency, start, the
   wait, the result, the sum, again or not) move out of `Radio.cpp` into a function a host test
   can drive, as `armScopeSegment` is for the scope: an interface with one member per call of
   RadioLib's, the clock handed in, `Radio.cpp` the chip half and nothing else. `stop` and any
   other command that arrives during a scan are served as they are today (`inputPoll()` in the
   wait).
4. **The numbers written once.** 8.2 µs a sample is one constant: the earliest a scan can be
   over, the deadline it is given up at (today `samples / 100 + 100` ms, beside a comment) and
   the words all come from it. Three scans of 65 535 samples are 1.6 s; say in the report why
   the watchdog's 60 s and the loop's own watch are not near.
5. **Tests**, in `test/test_scanner` with a fake of the new seam:
   - **the chip of 21 November:** a status that still reads *completed* from the scan before,
     a result that holds 15 samples when read at once and 2 048 when read after 16.8 ms.
     Sixteen frequencies give sixteen `hist` lines that each add up to 2 048. This test fails
     on the sequence as it is today; show that in the report;
   - a result with one bin of `0x1000` on the first read and a right one on the second: one
     `hist` line, the right one;
   - a chip whose result never adds up: three scans, one `error` line with the frequency, the
     sum and `n`, the pass ended, no `hist` line;
   - a status that never completes, and a start that fails: as today, by name;
   - `stop` between two frequencies and during a wait; a second pass; `samples` at its least
     and at its most.
6. **The words.** `docs/noisescope.md` §5: what `samples` buys (17 ms of one frequency at the
   default, 0.54 s at 65 535), that a line's bins add up to its `n`, and the `error` line a
   result that does not add up gets. §7: one or two sentences on what a `hist` is for beside a
   scope, with those times in them. A line in `release-notes/unreleased/`.
7. **The bench**, as lines in `docs/bench.md` §9 for a person with the board on USB, not run by
   this session: `hist 869.418 869.818 25` gives sixteen lines that each add up to 2 048 and
   are at least 17 ms apart by their `ms`; `hist 869.618 869.618 25 65535` gives one line that
   adds up to 65 535, about 0.55 s after its `ack`.

## Acceptance criteria

- [ ] The test of 21 November exists and fails without the change; with it sixteen lines each add up to 2 048 (the test's name, and the failing run quoted).
- [ ] No `hist` line is written whose bins do not add up to its `n`: the fake that never adds up gets three scans, one `error` line and an ended pass (the test's name).
- [ ] A *completed* read sooner than `n` × 8.2 µs after the start is not acted on (the test's name), and 8.2 µs is one constant.
- [ ] `stop` still ends a `hist` between two frequencies and inside a wait (the tests' names).
- [ ] `sweep`, `scope`, `dwell` and `cad` are what they were: their tests pass unedited.
- [ ] The report quotes RadioLib's start, abort and status functions and says in one sentence whether the cause was found or the door was closed without knowing it.
- [ ] Nothing under upstream's tree, `spec/` or `.pio/` was edited.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The Walter keeping seven of sixteen lines: its pile 18, task 03. Pacing `hist` for the port.
The picture on meshmaster.dk (its pile 4.47). A patch of RadioLib: what was learned about its
example goes in the report. The repeater role. The board: no session opens a port. A release:
Thomas presses the button.
