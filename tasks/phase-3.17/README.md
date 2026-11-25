# Phase 3.17 — what the garden's sessions change

Written on 25 November.

**What happened.** Phase 3.16 hunted the bright blocks of a wide `scope` on the unit at
Bellevue: sessions 6 to 9 from meshmaster.dk, each restored. There are three artefacts and
not one (`tasks/phase-3.16/FOLLOWUPS.md`, *What the garden said*):

| artefact | what the sessions show | state |
|---|---|---|
| a block at each segment's first point | follows the segments, at every bandwidth | **cured by a settle of 500 µs** |
| far echoes at fixed frequencies (816.0, 845.0, 887.7, 912.0 MHz for a signal on 869.618) | strong at 467 and 373.6 kHz, weaker at 234.3, all but gone at 58.6 kHz and below | cause not known; not the DC-DC, not the gain, not a lost lock |
| a raised floor around a strong signal | about 35 dB under it; 10 MHz either side at 467 kHz, 4.4 MHz at 117 kHz and below | the receiver's own |

**Since then (sessions 10 and 11, the same day):** the far echoes are the sweep plugin's
and not the chip's. The receiver parked on 912.0 MHz, and a host-stepped `sweep` across it,
never showed the neighbour; and an echo is gone when a segment begins at its place and
stands the settle (816 and 912 MHz from -64.5 and -61 dBm to the floor; 845 the same). The
plugin reads within some 10 µs of writing a frequency, and at certain frequencies the
synthesizer needs 30 to 40. The first row of the table and the second are one thing.

**What this pile builds.** 01 makes the settle the default: a `scope` that names no settle
stands 500 µs on each segment's first point. 02 adds a second way to sweep, `stepped`: every
point tuned with the chip's own command and stood on before it is read, the plugin left as
it is. The unit then says which way a wide picture should be made.

**Chosen by me, his to change before the run:**

- **500 µs.** 2 000 was no better and costs more: 29.1 sweeps a second with none, 28.0 with
  500, 25.8 with 2 000, for two segments at 467 kHz.
- **`0` is how none is asked for**, so every line typed before still parses.
- **The `ack` names the settle on every `scope`**, so a capture says how it was made.
- **A second way to sweep, not a list of places to cut at.** Cutting works where it was
  tried, but two of the four places fit no rule and a list would be one chip's.
- **`stepped` is asked for by a word and is nobody's default yet.**

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-settle-by-default](01-a-settle-by-default.md) | — | a `scope` that names no settle stands 500 µs on each segment's first point; `0` asks for none; the `ack` says which; the words on what a wide picture cannot tell |
| 02 | [a-scope-the-host-steps](02-a-scope-the-host-steps.md) | 01 | `scope … stepped`: every point tuned with the chip's own command, settled and read once; the same frames; the list `garden-e` that sets the two ways side by side |

```powershell
.\tasks\run-all.ps1 -Phase '3.17'
```

No board is needed.

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **Release**, import, mark it canary for Risskov LTE, *Fetch a scanner image* on the unit.
  The repeater's own image does not change in this pile.
- **Two sessions:** 01's two lines, and 02's `garden-e`. I start and read them.
- **Then the wide picture:** if `stepped` is clean and fast enough, meshmaster.dk's proposal
  types it for the wide span (a task there, after the session).

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
