# 02 — A segment the synthesizer can hold

**Depends on:** 01 (its tools, and its report's tables when a bench answered).
**Spec:** `tasks/phase-3.16/reports/01.md` (the plugin as read; the tables); `docs/noisescope.md` §5 (the `scope` command and its `ack`: *a `scope` ack carries what was taken*), §6 (*Why a wide span is several sweeps*) and §8 (*The scope across the band*: what the first board taught about the image calibration); `src/roles/scanner/Sweep.h` (`kScopeSegmentMhz` ~L288, `kScopePluginMaxPoints` ~L280, `kScopeSegmentUpdates` ~L315, `kScopeSinkDutyDivisor` ~L320, `ScopeRequest`, `ScopePlan` ~L400, `planScope` ~L418, `ScopeArmRadio` and `armScopeSegment` ~L486) and `Sweep.cpp` (`armScopeSegment` ~L359, the port's interval ~L395–404, `scopeSweepMs` ~L413, `planScope` ~L425); `src/roles/scanner/Radio.cpp` (`ChipArmRadio` ~L443–459, `serviceScope` ~L501, `startScope` ~L665); `src/roles/scanner/Commands.cpp` (the `scope` verb: ~L59, its usage ~L97, its numbers ~L130–143); `src/roles/scanner/Lines.cpp` (the `ack` of a scope); `src/roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumScanner.h` (`setSweep` ~L130–162: receive at the segment's centre, then the plugin's own jump to point 0); `test/test_scanner/test_scanner.cpp`; `tools/noisescope/echoes.py` and `hunt.py` (task 01); `docs/bench.md` §0a, §9
**Size:** one session.
**Model:** claude-opus-5

## Goal

Task 01 says where a wide `scope`'s echoes are and what they follow. If it is as the two
captures of 23 November read, there are two of them and each has an obvious cure to try:

- **the block at a segment's first point**: the receiver is put in receive at the segment's
  centre, the plugin jumps half a segment to point 0 and reads at once. At 467 kHz a reading
  is 3.4 µs and the allowance for that jump is a count of readings, not a time. *Cure to try:
  the receiver stands on the segment's first point for a time that is asked for, before the
  first reading.*
- **the block every 5.6 MHz or so after it**: something in the synthesizer gives way each
  time the sweep has dragged it that far. *Cure to try: a segment no wider than that.*

This task gives `scope` the two numbers that make both cures something a person can ask for,
and, where the bench has a carrier on it, measures what each does. **What `scope` does when
the numbers are not given does not change**: which numbers become the rule is decided after
the tables, by Thomas.

If task 01's report shows something else than the README reads, build what its tables point
at instead, say so in the first paragraph of the report, and keep the rule of this task: the
default does not move.

## Deliverables

1. **Two more numbers on the line.**
   `scope <f0> <f1> [points [rbw_khz [interval_ms [agg [seg_mhz [settle_us]]]]]]`
   - `seg_mhz`: the widest a segment may be. `0` or absent is 32, today's. From one step up
     to 32; a tenth of a MHz is taken. Anything else is refused in a sentence, as the other
     numbers are.
   - `settle_us`: how long the receiver stands in receive **on the segment's first point**
     after it is armed and before the first reading counts. `0` or absent is today's arm. Up
     to 20 000. How it is made to stand there is yours (a sweep of one point whose reading is
     thrown away is one way that uses nothing the chip has not been seen to do); say which
     and why in the report.
2. **The `ack` says what was taken**, and for a `scope` asked for without the two numbers it
   is what it is today, to the byte. With them it also names the segment's width and the
   settle. A test holds both.
3. **The plan knows.** `planScope` cuts by the width asked for; `kScopeMaxSegments` still
   bounds it; the time a sweep is planned to take includes every arm and every settle, so
   that the `ack`'s `sweep` and the interval the Walter's port is given stay true. The
   port's rule (a frame takes at most half the port's time) does not move.
4. **An arm does not calibrate what is calibrated.** Today every segment is armed with an
   image calibration for its own range, which is what 32 MHz was chosen for. A sweep of
   thirty-three segments must not pay thirty-three calibrations: a segment that lies inside
   the range the last calibration was made for is armed without one, and the calibration is
   made for 32 MHz at a time as today. `armScopeSegment`'s order, its answer to a refused
   calibration and its reload stay as §8 says and as their tests hold them; the new case goes
   through the same seam and is host-tested there. If the chip turns out to need the
   calibration at every arm, say what it said and leave it in.
5. **Host tests:** the plan for widths of 32, 11, 5.5 and 2 MHz over 180 MHz at 467 kHz and
   over 13 MHz at 58.6 kHz (points a segment, segments, every point in exactly one segment,
   the planned time); the arm's calls in order with and without a settle and with and without
   a calibration, through `ScopeArmRadio`; a width below one step, above 32 and a settle
   above the bound refused; the line with six numbers is what it was.
6. **Asked of the board**, when there is one (the README's bench; the scanner and nothing
   else; no `bench restore`). With `hunt.py` and a list this task adds
   (`tools/noisescope/lists/p3.16-02.txt`), each line for the 180 MHz around the carrier at
   467 kHz and again for 13 MHz at 58.6 kHz:
   - today's line, for the run's own reference;
   - `seg_mhz` 11, 5.5, 4 and 2 with no settle: do the blocks after the first point go, and
     at which width?
   - `seg_mhz` 32 with `settle_us` 100, 200, 500, 1 000, 2 000 and 5 000: does the block at
     the first point go, and at which settle?
   - the best width with the best settle, and one step either side of each;
   - for every line: the echoes left (count, the strongest below the carrier), `sps`, and
     whether the carrier is on its own point.
   One table, and under it **the two numbers the bench speaks for** and what they cost in
   sweeps a second against today's. If nothing removes them, say that, with the table.
7. **The captures behind the table are kept** in `tools/noisescope/captures/p3.16-02/`, as
   task 01 keeps its own.
8. **The words.** `docs/noisescope.md` §5: the two numbers in the command's row and in the
   `ack`'s paragraph; §6: what a segment's width and a settle are for; §7: what to type for a
   wide span, once the bench has said. `docs/bench.md` §9: the list as one line, and the
   image check's line brought up to date. A line in `release-notes/unreleased/`: a `scope`
   can be asked for with a segment width and a settle.

## Acceptance criteria

- [ ] `scope` with six numbers or fewer plans, arms, acks and sweeps exactly as on `main` (the tests that hold it, unedited, and one new one for the `ack`'s bytes).
- [ ] The two numbers are taken, bounded and refused as deliverable 1 says (the tests' names).
- [ ] The plan's time includes the arms and the settles, and the port's interval follows it (the test's name, with the numbers for 33 segments at 467 kHz).
- [ ] A segment inside a calibrated range is armed without a calibration; a refused calibration is still answered as §8 says (the tests' names).
- [ ] With a bench: deliverable 6's table, the two numbers it speaks for, their cost in `sps`, and the captures kept. Without: *Not verified here* and the one command.
- [ ] The report says what the default would become if Thomas takes the numbers, and that it has not been changed.
- [ ] `tools\verify.ps1` green. CI green.

## What only a board can show

Deliverable 6, and after it the unit: a session on Bellevue with the wide span typed with the
two numbers (`... 1000 max <seg> <settle>`), next to one typed without. Thomas's, from
meshmaster.dk; I read both with `echoes.py`.

## Out of scope

Making the numbers the default, and meshmaster.dk's proposal typing them: both after the
tables. The plugin's words and the latency's table. A sweep from high to low (the plugin's
step has no sign). Two layouts and the lower reading kept: in the follow-ups, for after this.
The Walter. A release.
