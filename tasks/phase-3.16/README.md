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

## The garden instead of the bench (24 November, evening)

Thomas would rather not take the unit down, and he does not have to. **The echoes have only
ever been seen on the unit at Bellevue**, with its antenna, its cavity filter and the
neighbour 200 m away; the bench has never shown one. A NoiseScope session from meshmaster.dk
types `scope` lines and brings every line back, so the unit is where they are measured.
Tasks 03 and 04 are written for that and need no board. (The bench of *Before the run* never
needed the unit: any RAK4630 on USB does. It stays for what the garden cannot give.)

**What the garden cannot give:** a signal of a known level in every frame, the dummy load,
an attenuator. The neighbour's traffic lights the echoes in some frames and not in others
(ten of sixty in session 3), so a step is long and each frame is the strongest reading of a
second.

**What a session costs:** Bellevue off the mesh for about 27 minutes (two flashes, nine
minutes of steps, the proof), about 0.3 MB of the SIM's month, and a new scanner image is
one fetch of 127 kB. Since Walter 0.1.11 the first try of the scanner flash has failed in
every session (four of four); the second try, which 0.1.12 has, carried two of three. A
session that is refused measures nothing and costs the wait.

**What is on the unit** (read 24 November, 21:35 CET): Walter 0.1.12, the radio's golden
image and its scanner both 0.1.7. meshmaster.dk has pile 4.48 since 20:50 CET, so a refused
session is kept and holds no other result back.

**A step's line is at most 40 characters** and a program at most six steps and 384 bytes
(meshmaster.dk's contract and the Walter's). So the lists use whole MHz, leave out `max`
where no number follows it, and 03's words may stand anywhere.

### Session A: does an echo follow the segments or the signal? (needs nothing new)

It runs on the scanner that is on the unit. Six steps of 90 seconds:

```
90 scope 780 960 0 467 1000
90 scope 773 953 0 467 1000
90 scope 777.2 957.2 0 467 1000
90 scope 850 890 0 467 1000
90 scope 862 875 250 58.6 1000
90 scope 865 878 250 58.6 1000
```

Steps 1 to 3 are the wide span begun 0, 7 and 2.8 MHz lower: echoes that follow the segments
move down by 7 and by 2.8 MHz, echoes that follow the signal stay where they are. Step 4 is
40 MHz around the signal in two segments. Steps 5 and 6 are the 13 MHz capture begun at 862
and at 865 MHz: an echo that follows the segment is at 873.2 and then at 876.2 MHz, one that
follows the signal is near 874.4 MHz in both. This is the question the two readings differ
on, and it is answered before any firmware is built.

### Sessions B, C and D (need a release with 02 and 03, and *Fetch a scanner image*)

85 seconds a step. Task 04 turns them into files and checks them against the rules.

**B, the regulator, the gain and the time a point costs:**

```
85 scope 780 960 0 467 1000
85 scope 780 960 0 467 1000 ldo
85 scope 780 960 0 467 1000 noboost
85 scope 863 876 250 58.6 1000
85 scope 863 876 250 58.6 1000 ldo
85 scope 869 870.2 250 4.8 1000
```

Echoes that are gone on the LDO are the DC-DC converter's. Echoes that fall by more than the
signal does at the lower gain are the receiver being driven too hard. The last step is 250
points at 4.8 kHz in one segment: its `us` reads about 83 ms if an update is 1.6 ms / RBW
and about 48 ms if it is 0.94.

**C, a segment's width** (60 MHz around the signal, where the strongest echo was, 887.1 MHz):

```
85 scope 840 900 0 467 1000
85 scope 840 900 0 467 1000 max 11
85 scope 840 900 0 467 1000 max 5.6
85 scope 840 900 0 467 1000 max 4
85 scope 863 876 250 58.6 1000
85 scope 863 876 250 58.6 1000 max 5.6
```

**D, the settle on a segment's first point:**

```
85 scope 840 900 0 467 1000
85 scope 840 900 0 467 1000 max 32 100
85 scope 840 900 0 467 1000 max 32 500
85 scope 840 900 0 467 1000 max 32 2000
85 scope 863 876 250 58.6 1000
85 scope 863 876 250 58.6 1000 max 32 1000
```

Every session begins with the span asked as today, because the traffic of one hour is not
the traffic of the next. `dev` is in every frame of more than one segment.

**Chosen by me, his to change before the run:**

- **The words are on the `scope` line**, taken out wherever they stand: a session cannot say
  `boost off`, and 40 characters leave no room for a word that needs numbers before it.
- **`dev` is read where the arm already stands in standby**, and a span of one segment is
  not given a standby to read it in: nothing of what is measured moves.
- **The neighbour is the signal.** A steadier one would make every step sharper: the tinySA
  as a generator with a short antenna a few metres from the unit, on 869.618 MHz, for the
  nine minutes of steps. It also sits on the mesh's channel for those minutes. Not needed for
  session A.
- **Four sessions, not one long one:** six steps and 600 seconds are the contract's, in two
  other repositories.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-echoes-are-measured](01-the-echoes-are-measured.md) | — | `echoes.py`: every echo of a capture with its distance from its segment's first point in MHz, points and microseconds, and which of the three repeats; the plugin read; `hunt.py` and the bench's answers to where, what they follow and how long |
| 02 | [a-segment-the-synthesizer-can-hold](02-a-segment-the-synthesizer-can-hold.md) | 01 | `scope` takes a segment's width and a settle; no calibration for a range already calibrated; the bench's table of what each width and settle leaves and costs; the default as it was |
| 03 | [a-scope-that-accounts-for-itself](03-a-scope-that-accounts-for-itself.md) | 02 | `scope` takes `ldo`/`dcdc` and `boost`/`noboost` for one scope; every frame says how long its sweeps took (`us`, `usx`) and, with more than one segment, what the chip's error word held (`dev`); a scope asked as today measures as today |
| 04 | [the-garden-is-the-bench](04-the-garden-is-the-bench.md) | 03 | `hunt.py --sessions`: a list as session programs meshmaster.dk will take; the four garden lists; `echoes.py --steps`: one table across a session's scopes and what each pair of them says |

```powershell
.\tasks\run-all.ps1 -Phase '3.16' -From 03
```

01 and 02 have run (24 November, with no board on the bench). 03 and 04 need no board.

## Models

- **Opus**: 01, 02, 03, 04.

## Afterwards (Thomas)

- **The garden** (03 and 04): session A whenever he likes, it needs nothing new. After 03
  has merged: release, import, *Fetch a scanner image* on the unit, then sessions B, C and D,
  one at a time. Each download saved into `tools/noisescope/captures/garden/` under its own
  name; I run `echoes.py --steps` over it and say what it shows. 04 changes nothing a unit
  runs, so the release need not wait for it.
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
