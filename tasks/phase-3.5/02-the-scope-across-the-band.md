# 02 — The scope across the band

**Depends on:** 01.
**Spec:** `docs/noisescope.md` §5 (`scope`, its `ack`, its frames and its `err` line), §6 (*Design notes*: the image calibration, the hardware reset when a sweep ends, the checks before every sweep), §8 (*The fast sweep, as built*); `docs/bench.md` §9 — 9.9 *The baseline* and 9.11 *The site through the cavity filter* — and *The fast sweep*; `src/roles/scanner/Radio.cpp` (`armSegment`, ~L357–369, the comment above it on the order of the calls, and everything that runs between two segments), the scope's plan (`ScopeSegment`, `kScopeSegmentMhz`, how `cal_lo_mhz`/`cal_hi_mhz` are chosen); RadioLib at the version `platformio.ini` pins, on disk after a build (`SX126x::standby`, `SX1262::setFrequency` and `RADIOLIB_SX126X_CAL_IMG_FREQ_TRIG_MHZ`, `SX126x::calibrateImageRejection` / `calibrateImage`, the status check that yields `RADIOLIB_ERR_SPI_CMD_FAILED`, −707, `TypeDef.h:440`); the in-radio sweep plugin as phase 3.1 brought it in (MeshCore #3578, `rom` `SX1261 V2D 2D02`, `setSweep`); [`../phase-3.1/FOLLOWUPS.md`](../phase-3.1/FOLLOWUPS.md), *From a bench walk of section 9* (the table of eleven spans — this task's starting point), `tasks/phase-3.1/reports/01.md`, `tasks/phase-3.4/reports/02.md`; `out/bench/walk-9-*.md` and `out/bench/baseline-*.ndjson` from 22 October.
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

On the first real radio, `scope` streams over one or two segments and not over four or more. The
third segment's arm fails with −707 (`RADIOLIB_ERR_SPI_CMD_FAILED`), or the stream goes silent with
no `err` at all. 863–870 MHz, the span the fast sweep exists for, is one segment and works at about
40 sweeps a second. 150–960 MHz (the baseline, 26 segments) and 791–960 MHz (the site through the
cavity filter, six segments) do not work. Find which call fails and why, then make a wide span stream.

## Deliverables

1. **Name the call, for good.** `armSegment` makes four calls and returns the first error. The `err`
   line gives the frequency but not which call failed. Add the call and its code to the `err` line,
   additively (`docs/noisescope.md` §5 says so: a reader that ignores the new field is unaffected).
   Keep it permanently, because it is what a report from the field needs. Native test.
2. **Narrow it down on the board,** with `bench console --scanner` (the scanner is flashed back the way
   3.4-02 found, in two DFUs until 03 lands). Reproduce the table first. Then answer:
   - Which segment index fails first?
   - Does it depend on the number of segments, or on what the segment before did? Two segments then a
     third, against a span that starts at the third segment's frequency.
   - Which call returns −707: `standby`, `setFrequency`, `calibrateImageRejection`, or the plugin's
     `setSweep`?
   - Does a `stop`, or the radio's reset between segments, clear it?
   - Where does the silent case (300–400, 500–600) stop?

   Read RadioLib and the plugin from disk, not from memory. Every probe's command and result goes into
   a table in the report.
3. **Fix it in our code.** Not in RadioLib's, and not in the plugin's. If the bug is in a pinned
   library, that is a finding: cite it and make the smallest workaround in ours. The reading may
   confirm or rule out these candidates; this is not a prescription:
   - the CalibrateImage parameters for a range outside Semtech's five presets;
   - the chip still busy, or a calibration still running, when the next command arrives;
   - the plugin's state not surviving an image calibration or a frequency move;
   - the order of the four calls, which the comment above `armSegment` argues for;
   - what the end-of-sweep reset of §6 does to the next arm.

   Where the fix is logic (the plan, the order of calls, a wait), cover it with native tests behind a
   fake radio.
4. **Walk it:** `bench walk 9` on the board.
   - 9.9 must come back *ok* with frames in the kept NDJSON. Draw the picture if
     `tools/noisescope/requirements.txt` installs; if not, the NDJSON is the evidence.
   - `scope 791 960` must stream on the dummy load. What it shows through a cavity filter on a mast
     stays a person's to read.
   - `scope 863 870` must still stream at the rate phase 3.4 measured.
   - Then `--tick`.
5. **If the cause is not found in this session, land what was learned:**
   - the failing call named in the `err` line;
   - the probe table;
   - the widest span that streams;
   - a follow-up tagged for whoever must take it next: a RadioLib issue, the plugin's author, or a
     probe Thomas must make with the scope on the SPI lines.

   **No span is refused or silently clipped to hide the fault.** An `err` line is the honest answer.
   The task lands either way.

## Acceptance criteria

- [ ] The `err` line names the call and its code (native test; `docs/noisescope.md` §5).
- [ ] The probe table from the board is in the report.
- [ ] `scope 150 960 2048` streams on the board (9.9 *ok*, NDJSON kept), or the report says exactly where the fault is and why it is not fixed here.
- [ ] `scope 863 870` streams no slower than phase 3.4 measured.
- [ ] The board restored to the repeater image with its identity (walk 9's own last step).
- [ ] `tools/verify.ps1` green.

## Not verified here

9.10 (the image check with a generator) and the reading of 9.11 on a mast: Thomas, with the tinySA.

## Out of scope

The watchdog (03). Lowering `kScopeSegmentMhz` belongs to the image check (9.10, Thomas), unless the
fix needs it; if it does, the report says so in its own paragraph.
