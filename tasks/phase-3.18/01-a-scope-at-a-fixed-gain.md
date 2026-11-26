# 01 — A scope at a fixed gain

**Depends on:** — (phase 3.17's 02 is on `main`: the words on the line and the arm this adds to).
**Spec:** `tasks/phase-3.17/FOLLOWUPS.md` (*Session 13*, *Session 14*); `tasks/phase-3.18/README.md`; `docs/noisescope.md` §5, §6 and §7; `src/roles/scanner/Sweep.h` (`ScopeGain` ~L413, `scopeRadioSetup` ~L441), `Sweep.cpp` (`scopeGainOf` ~L346, `scopeAckMessage` ~L784), `Radio.cpp` (`beginPlugin` ~L456, where the gain is applied ~L159–170, `ChipArmRadio`), `Commands.h` (`kMaxTokens` ~L34), `Commands.cpp` (`parseScope`); Semtech's data sheet for the SX1261/2, Rev 2.2, section 6.1.6 and table 6-4 (read by the session at Semtech's; nothing of it copied into the repository).
**Size:** one session.
**Model:** claude-opus-5

## Goal

When the neighbour of the unit at Bellevue sends (-34 dBm at the radio, from 200 m), every
reading within about 4 MHz of it rises from the floor to about -70 dBm. Session 15 of 26
November, in the frames holding that signal: 250 of 250 points of the 2 MHz picture, 195 of
250 of the 13 MHz picture, 17 of 386 of the 180 MHz picture. A second station on the same
channel at -65 dBm raises nothing beside itself: the floor stands on both sides of it. The
width is the same in MHz at 58.6, 46.9, 39 and 9.7 kHz, and it is there the plugin's way and
`stepped` (sessions 8, 12 and 14). So it is not the sweep.

**The reading, not proven:** the chip's gain control. With a signal that strong inside its
analogue filter it turns its gain down, and whatever it measures meanwhile it measures with a
higher floor. The data sheet (Rev 2.2, 6.1.6 and table 6-4) names a gain control with
thirteen gains and a *manual gain mode* in which it is switched off. It does not say how that
mode is entered.

This task gives `scope` a word that holds the receiver at one gain for that scope, so that
the unit can say whether the rise is the gain control, and which gain a site with a loud
neighbour is measured at. Thomas, 26 November: *turn down the gain a lot and see if the
smearing stops … maybe as a parameter such that it can be done if one has a loud repeater
nearby.* **It is a word on the line and nobody's default. A `scope` without it measures as
today.**

There is no board in this session. Build it so that the first session on the unit is the
proof, and say plainly in the report what that session has to show.

## First: how the chip is told

1. **Find, in what Semtech itself has published, how an SX1261/2 is put into manual gain
   mode and how one of its gains is chosen**: its driver and examples (the `Lora-net`
   repositories on GitHub), its application notes, the data sheet. Name the source in the
   report and beside the code: repository, file, line and commit, or document, revision and
   section (`docs/upstream.md`'s rule for a line of somebody else's). The pinned RadioLib
   (7.8.0: the receive gain register and its three values, `resetAGC()`) is a second
   source, not a first.
2. **If nothing of Semtech's says how, stop: merge the report alone.** It says where it
   looked and what it found. No register is written on a guess, and a third party's code
   with no word of Semtech's behind it is described in the report and left for Thomas.
3. Read with it, and say in the report: which number is the most gain and which the least;
   whether the mode is the same in the GFSK receiver that the plugin and `stepped` both
   use; **what a level means at a held gain** (6.1.6 says the reading is corrected for the
   gain from per-gain values that are nought for this band: is it still corrected for the
   gain that is held?); what `boost`/`noboost` still does beside it; whether anything of it
   outlives a reset of the chip.

## Deliverables

1. **The word `gain<N>` on the `scope` line**: `gain7`, one word, letters then the chip's
   own number of the gain as the source numbers them. Taken out of the line first, wherever
   it stands after the two ends, as `ldo`, `noboost` and `stepped` are; what is left is read
   exactly as today. Once at most, and a number the chip does not have is refused in a
   sentence that names the range. It stands beside `boost`/`noboost` unless the source says
   the two cannot be had together, in which case that is refused in a sentence too.
   `kMaxTokens` grows by what this needs.
2. **For this scope and nothing after it.** When the scope ends, however it ends, the radio
   is what it was for the mode that follows; a test holds that a `sweep`, a `hist`, a
   `dwell`, a `cad` and a `scope` without the word after such a scope begin the chip as they
   do on `main`.
3. **Held through the whole scope, both ways of sweeping.** Applied where the receiver is
   put in receive, and again wherever an arm, a reload of the plugin or a calibration would
   undo it, for the plugin's sweep and for `stepped`. Through the `ScopeArmRadio` seam, in
   `armScopeSegment`'s order, host-tested for a first segment, a later one, a refused
   calibration and a reload.
4. **The `ack` says what was taken:** `held=<N>` after `mode=` and before `reg=`. For a
   `scope` without the word the `ack` is today's line, to the byte. A frame does not change.
5. **What a level is worth then, in the words and not in the firmware.** If the reading is
   not corrected for the held gain, a held scope's levels are off by one amount for its
   gain: the firmware corrects nothing (there is no table for this board), `docs/noisescope.md`
   says so, and the garden's list measures it (the neighbour reads -34 dBm with the gain
   control on).
6. **`tools/noisescope/echoes.py --steps` reads `held=`**: a column of the table, and for a
   scope **the rise**: in the frames holding the signal (`--carrier`, `--lit`), how many of
   the scope's points stand `--over` dB or more over the same point's median in the quiet
   frames, and the median of the span in the lit and in the quiet frames. Two scopes that
   differ in the held gain alone are a pair, with one line: the rise of each and the quiet
   floor of each. Counts and medians, nothing learned from the data.
7. **The garden's list `tools/noisescope/lists/garden-f.txt`**, taken by `hunt.py
   --sessions`, six steps of 85 s, every line 40 characters or fewer:
   `scope 868.6 870.6 250 0 1000` as it is asked today (the reference), the same at four
   held gains spread over the chip's range (the most, two between, the least), and
   `scope 863 876 250 0 1000 stepped` at one of the middle ones. A comment over each line
   says what it asks.
8. **Host tests:** the word in every position, alone and with each of the other words;
   twice, and out of range, refused; ten lines of today's, word for word what they parse to
   on `main`; the `ack`'s bytes with and without; the arm's calls in order with the gain's,
   both ways of sweeping; the radio as it was after such a scope; planted captures for the
   rise with the gain control on and held, and the pair's line; every example line 40
   characters or fewer, and a test says so.
9. **The words.** `docs/noisescope.md` §5 (the word in the command's row, the `ack`), §6
   (why a scope can be asked for at a held gain: a strong signal nearby; what a level means
   then; that it is for a site with a loud neighbour and no default), §7 (`garden-f`). The
   `help` text. A line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The report names where Semtech says how manual gain is entered and a gain chosen (repository, file, line and commit, or document, revision and section), and the same stands beside the code; or the report alone is merged.
- [ ] A `scope` without the word plans, arms, acks and sweeps as on `main` (the tests that hold it, unedited where they are about the plan and the `ack`).
- [ ] The word is taken wherever it stands, refused twice or out of range, and the radio is what it was when the scope ends (the tests' names).
- [ ] The gain is applied in the arm's order for both ways of sweeping, at a first segment, a later one and a reload (the tests' names).
- [ ] `echoes.py --steps` gives the rise for a planted capture with the gain control on and with a gain held, and the pair's line (the tests' names).
- [ ] `garden-f.txt` is taken by `hunt.py --sessions`; its program is printed in the report.
- [ ] Nothing that transmits and nothing that writes flash was added (`docs/noisescope.md`'s first lines); the report says how it checked.
- [ ] The report's *Not verified here* says what the first session on the unit has to show, one line each: the rise with the gain control on and at each held gain; the quiet floor at each; the neighbour's reading at each.
- [ ] `tools\verify.ps1` green. CI green.

## What only the unit can show

Whether the rise is the gain control at all. Which gain keeps the floor where it is and
still reads the neighbour. What a level is worth at that gain. After the release I start
`garden-f` from meshmaster.dk and read it.

## Out of scope

A default, and a gain chosen by the firmware itself: a site is said to have a loud
neighbour, nothing guesses it. meshmaster.dk's form (a choice there once the unit has said
which gain). The other modes. Correcting a level for the gain it was read at. The repeater's
own receiver. A session's forty characters. A release.
