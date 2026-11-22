# Phase 3.15 — a histogram that adds up

Written on 22 November.

**What happened.** The first NoiseScope session on a unit (21 November, Bellevue, meshmaster.dk
`/noisescope/1`) had one `hist` step: `hist 869.418 869.818 25`, sixteen frequencies of 2 048
samples. Every line says `"n":2048`. The bins of the first add up to 2 048. The six after it
that reached the server add up to 15, 15, 8, 4 140, 32 and 1 807.

**What the numbers say.** Take away one bin in each of two lines (4 096 and 1 792: `0x1000` and
`0x0700`, a high byte with nothing under it) and what is left is 15, 15, 8, 44, 32 and 15
samples. At 8.2 µs a sample that is a result read 70 to 360 µs after its scan was started. The
lines are 22 ms apart, and a scan of 2 048 samples is 16.8 ms before a line of 170 bytes takes
its 14.8 ms of the port.

**My reading, not proven.** `serviceHist` asks the chip for the scan's status right after it
starts one. The status still says *completed*, from the frequency before, so the result is read
at once. RadioLib's own example asks the same way, which speaks against the reading; no session
has a chip to ask. Task 01 is written so that it holds whichever the cause is.

**Chosen by me, his to change before the run:**

- **A `hist` line whose bins do not add up to its `n` is never sent.** The frequency is measured
  again, three scans at most, then an `error` line and the pass ends as the function's other
  failures do.
- **A scan is not taken as finished before its samples can have been taken**, whatever the
  status register reads.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-histogram-that-adds-up](01-a-histogram-that-adds-up.md) | — | a result belongs to the scan started for it; no `hist` line whose bins do not add up to its `n`; the sequence behind a seam and host-tested against the chip of 21 November; the words say what a `hist` line is a picture of |

```powershell
.\tasks\run-all.ps1 -Phase '3.15'
```

## Models

- **Opus**: 01.

## Afterwards (Thomas)

- **Release** with the button and merge its notes pull request. It carries pile 3.14 as well
  (a person may ask again), so the radio's image is worth applying too.
- **The chip's word.** Offer the scanner image to the unit, then a session with one typed
  step, `20 hist 869.418 869.818 25`. Sixteen lines that each add up to 2 048 is the answer;
  an `error` line that names a sum is the other answer. I read them off the page.
- **Until the Walter's pile 18 task 03 is on the unit** it keeps about seven of the sixteen
  lines. Seven that add up are still the answer.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
