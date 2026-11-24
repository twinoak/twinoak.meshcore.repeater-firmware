# Phase 3.15 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [x] **[Thomas] Nobody has seen task 01 on a chip.** The cause is read from numbers and from
  RadioLib's source. The answer is a session on the unit with one step,
  `20 hist 869.418 869.818 25`, after the release: sixteen lines that each add up to 2 048, or
  an `error` line that names a sum.
- [ ] **[later, Thomas to decide] RadioLib's example asks the status the same way.** If the
  session shows the register does keep *completed* across a start, that is a report for
  upstream. His to file.
- [ ] **[later] `hist` sends as fast as it measures.** At the least `samples` (256) a scan is
  2 ms and the port is full for the whole pass. The Walter's side has to keep up with a full
  port (its pile 18, task 03); nothing here slows a pass down.

## The review of 01 (22 November)

Merged 22 November (#59, main eaa19a7). Read: the report whole, `measureHistPoint()` and
`oneHistScan()` in `Sweep.cpp`, the chip half's members in `Radio.cpp`. Not read line by line:
the tests, `tools/bench/`, the docs. Eight of eight criteria. Nothing that holds a release back.

- [ ] **[none] What was built is the task, and one thing more.** A time floor under every
  *completed*, the result read once after it, the bins added, three scans at most, then one
  `err` line and the pass ends. More than asked: the status register is written back to *none*
  before a start (and not relied on: the test's chip ignores the write), and it is read raw so
  that *ongoing* can be told from *completed*.
- [ ] **[Thomas] The cause was not found**, as the report says in its first paragraph. The
  chip's word is still the session of this file's first item.
- [ ] **[later] A retune the driver refuses now ends the pass** (`scan retune failed`), where
  the return value was thrown away before. Right for a narrow `hist`. A `hist` whose steps are
  20 MHz or more goes through RadioLib's own image calibration at each step, the one the scope
  avoids with `skipCalibration` (`Sweep.h`, `ScopeArmRadio::setFrequency`); if the chip refuses
  it there, such a pass now stops at its second frequency. Not seen; nobody has typed one.
- [ ] **[none] The second bench line became two frequencies** (`hist 869.618 869.668 25 65535`):
  a `hist` of one frequency is refused by `planSweep()`, which the task's author did not know.

## The chip's word (23 November)

- [x] **[none] The histogram adds up on a chip.** NoiseScope session 3 on the unit at Bellevue
  (meshmaster.dk `/noisescope/3`, scanner image 0.1.7+eaa19a7) ran `20 hist 869.418 869.818 25`:
  sixteen lines, 869.418 to 869.793 MHz, `n` 2048 each, and the bins of every one of the
  sixteen add up to 2 048. No `err` line. The pass took four seconds. On 21 November six of
  the seven lines held did not add up. The cause is still not known; the fix holds.

## The echoes in the wide scope (24 November)

Thomas asked a radio group about the wide picture of session 3
(`scope 779.618 959.618 0 467 1000`, 386 points, six segments). Worked out from that step's
sixty frames (meshmaster.dk `/noisescope/3`), nothing measured anew.

- [ ] **[3.16/01] The echoes follow the sweep's own structure and not the
  frequency.** Ten of the sixty frames hold a strong signal on the home channel (-32 to
  -50 dBm at 869.6 MHz; a neighbour 200 m away transmits there). In those frames and no
  others, other points rise by 14 to 35 dB, each with a tail in the sweep's direction:
  **at the first point of a segment** (840.5 MHz segment 3, 900.2 segment 5, 930.0 segment 6,
  fading over 5 to 15 points) and **at points 12, 24, 36 and 48 of a segment** (815.8 MHz is
  segment 2 point 12; 911.4 segment 5 point 24; 887.1 segment 4 point 36; 832.5 segment 2
  point 48). In the quiet frames the same places read 4 to 8 dB over the floor (also 846.1,
  935.6 and 941.2 MHz: points 12, 12 and 24), so the faint lines there cannot be called
  signals either. Outside 858 to 879 MHz nothing in that picture is known to be real.
- [ ] **[3.16/01, 3.16/02] What the firmware has to work with.** An RSSI update at 467 kHz is about
  3.4 µs, a point is one update, and the segment's allowance (`kScopeSegmentUpdates`, 8) is a
  count and so 27 µs there against 2.7 ms at 4.8 kHz. The plugin's host class takes the
  updates skipped after a retune, the delay between reads and the reads before it gives up
  waiting for a change (`vendor/sx126x_spectrum/SX126xSpectrumScanner.h` ~L76, ~L145;
  `spectrum.asm` ~L25–37, `wait_update`). What happens every twelve points is not read.
  Candidates for a task: the settle after a segment's arm by time and not by count; the
  twelve-point pattern read against `spectrum.asm`; a second layout of segments and the lower
  reading of the two kept. The bench is a RAK4630 on USB with a strong carrier beside it.
- [ ] **[none] The image calibration is already made for each segment** (the advice given to
  him), and the order of points inside a segment is the chip's.
