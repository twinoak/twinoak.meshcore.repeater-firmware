# Phase 3.16 — the echoes in a wide scope

Written on 24 November.

**What happened.** Thomas showed the wide picture of NoiseScope session 3 (Bellevue,
23 November, `scope 779.618 959.618 0 467 1000`) to a radio group: bright blocks at
frequencies where nothing can be, behind a cavity filter that passes 863 to 870 MHz. The
advice he got was to find out whether they follow the frequency or the place in the sweep.

**What the two captures say** (session 3's lines, worked out by hand, 24 November). A neighbour
200 m away transmits on 869.618 MHz and reads -32 dBm at Bellevue. In the frames that hold
it:

| capture | where a block begins | from its segment's first point | strongest |
|---|---|---|---|
| 180 MHz, 467 kHz, 386 points, six segments | 840.5 and 930.0 MHz (900.2, faint) | 0 | -61 to -70 dBm |
| | 815.8 MHz (846.1, inside the block before it) | 12 points, 5.6 MHz | -59 to -69 |
| | 911.4 MHz (941.2, faint) | 24 points, 11.2 MHz | -60 to -73 |
| | 887.1 MHz | 36 points, 16.8 MHz | -47 to -55 |
| | 832.5 MHz | 48 points, 22.4 MHz | faint |
| 13 MHz, 58.6 kHz, 250 points, one segment | 863.1 MHz | 0 | -57 to -63 |
| | 874.4 MHz | 216 points, 11.2 MHz | -64 to -67 |

The segments of the first begin at 779.9, 810.2, 840.5, 870.3, 900.2 and 930.0 MHz. Each
block fades over five to fifteen points in the sweep's direction. In the second capture the mark at
5.6 MHz lies under the neighbour's own skirt. In the quiet frames the same places read 4 to
8 dB over the floor. So the blocks follow the sweep's segments, not the frequency, and the
interval is the same in MHz at two bandwidths and two steps: about 5.6 MHz from wherever a
segment began. That is two captures from one unit. Nothing of it is measured on a bench.

**What is known of the cause.** The sweep runs inside the SX1262 (a plugin of 167 words, not
ours; its source is in the repository to be read). It retunes after every RSSI update and at
467 kHz an update is 3.4 µs. The receiver is armed at a segment's centre and the plugin jumps
to its first point and reads. What gives way every 5.6 MHz is not known: the synthesizer
changing band is my guess, and it is a guess.

**What this pile builds.** 01 measures: a tool that finds the echoes in any capture and says
what they follow, the plugin read, and the questions asked of the bench board. 02 gives
`scope` two numbers, a segment's width and a settle before its first reading, and measures
what each does. **Neither changes what `scope` does when it is asked as today.**

**Chosen by me, his to change before the run:**

- **Measured before mended.** The default moves in a task after this pile, from the tables.
- **The two numbers are on the `scope` line itself**, so that a session on a unit can type
  them as well as the bench.
- **A carrier from the generator, not the neighbour off the air:** steady, in every frame,
  at a level that is known.

## Before the run (Thomas)

The tasks use the bench board (`bench.local.json`, the RAK4630 on USB) if it is there, and say
*Not verified here* if it is not.

1. **The board runs a scanner before anything but the dummy load is on its antenna port.**
   `python tools/bench/bench.py status` says the role. A repeater transmits; the scanner
   never does. If it says repeater, flash the scanner of the last release with
   `python tools/bench/bench.py flash <its package>` first.
2. **Then the generator in place of the dummy load:** the tinySA Ultra, a plain carrier on
   869.618 MHz, with attenuation so that about -30 dBm reaches the radio, and never less than
   10 dB between the two. Left on for the whole run.
3. **`"rf": "antenna"` in `bench.local.json` for the run.** The tool then refuses a repeater
   image by itself. The tasks flash the scanner and nothing else and do not run
   `bench restore`.
4. Optional: the lines of `/noisescope/3` (*Download the lines*) saved as
   `tools/noisescope/captures/bellevue-2026-11-23.ndjson`. Task 01 holds its tool to them.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-echoes-are-measured](01-the-echoes-are-measured.md) | — | `echoes.py`: every echo of a capture with its distance from its segment's first point in MHz, points and microseconds, and which of the three repeats; the plugin read; `hunt.py` and the bench's answers to where, what they follow and how long |
| 02 | [a-segment-the-synthesizer-can-hold](02-a-segment-the-synthesizer-can-hold.md) | 01 | `scope` takes a segment's width and a settle; no calibration for a range already calibrated; the bench's table of what each width and settle leaves and costs; the default as it was |

```powershell
.\tasks\run-all.ps1 -Phase '3.16'
```

01 alone is `-To 01`.

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **The board:** generator off, the dummy load back, `"rf": "dummy-load"` back, and
  `python tools/bench/bench.py restore` if a repeater was on it before.
- **The tables decide.** I read both reports and say which two numbers they speak for and
  what they cost. Making them the default, and the proposal on meshmaster.dk, are the next
  task.
- **More of the same, one command each** if the tables leave a question: the carrier on
  865.0 MHz, 10 and 20 dB weaker, and the dummy load alone
  (`python tools/noisescope/hunt.py --label <name>`).
- **Release** only when 02 is wanted on a unit: a session can then type the two numbers.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
