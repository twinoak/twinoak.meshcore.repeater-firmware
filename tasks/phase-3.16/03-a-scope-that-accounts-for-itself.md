# 03 — A scope that accounts for itself

**Depends on:** 02 (the line it adds to, and the arm's seam).
**Spec:** `docs/noisescope.md` §5 (the `scope` command, its `ack`, the `scope` message), §6 and §8; `tasks/phase-3.16/FOLLOWUPS.md` (*The data sheet itself*, *Rev 2.2 of the data sheet, read against Rev 1.2*); `tasks/phase-3.16/reports/01.md` (the plugin as read) and `reports/02.md`; `tasks/phase-3.16/README.md` (*The garden instead of the bench*).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Two readings of the echoes are open: they begin about every 5.6 MHz from a segment's first
point, or they lie at multiples of about 5 MHz from the strong signal, which is where the
radio's DC-DC converter switches (it is clocked from the synthesizer in receive). And one
number this firmware is built on is in doubt: it takes an RSSI update to be 1.6 ms / RBW,
and Semtech's data sheet (Rev 2.2, table 13-82) says a value every 0.94 ms / RBW.

The bench board was not on the bench for 01 and 02, and the echoes have only ever been seen
on the unit at Bellevue. So they are measured there: a NoiseScope session from meshmaster.dk,
whose steps are typed lines. A session's step begins with one of six words (`scope` is one;
`boost` is not), its line is at most 40 characters, and nobody reads a serial port.
**Everything asked has to be on the `scope` line, and everything learned has to be in the
lines a scope sends.**

This task gives `scope` four words, for the regulator and the gain, and makes every frame say
how long its sweeps took and what the chip's own error word held. **A `scope` asked for as
today measures as today.**

There is no board in this session either. Build it so that the first session on the unit is
the proof, and say plainly in the report what that session has to show.

## Deliverables

1. **Four words on the line: `ldo`, `dcdc`, `boost`, `noboost`.**
   - They are taken out of the line first, wherever they stand after the two ends; what is
     left is read exactly as today (`points`, `rbw_khz`, `interval_ms`, `agg`, `seg_mhz`,
     `settle_us`, in that order). So `scope 780 960 0 467 1000 ldo` and
     `scope 780 960 0 467 1000 max 5.6 500 ldo noboost` are both lines. That is for the 40
     characters: a word must not need the numbers before it.
   - `ldo` | `dcdc`: the regulator the chip is begun with **for this scope**. Neither is the
     build's own (`beginPlugin`, `src/roles/scanner/Radio.cpp`; `SX126X_USE_REGULATOR_LDO`
     is defined nowhere today, so that is the DC-DC).
   - `boost` | `noboost`: the receive gain **for this scope**. Neither is what `boost on|off`
     left.
   - Both words of a pair on one line are refused in a sentence, as the other refusals are.
     `kMaxTokens` grows by what this needs. Anything else on the line means what it means
     today.
   - **When the scope ends, however it ends, the radio is what it was** for the mode that
     follows: the build's regulator, the gain `boost on|off` left. A test holds that a
     `sweep`, a `hist`, a `dwell` and a `cad` after such a scope begin the chip as they do on
     `main`.
2. **The `ack` says what was taken.** For a `scope` without the words it is today's line, to
   the byte. With one it also names it (`reg=ldo`, `gain=noboost`), after what 02 added.
3. **Every `scope` frame says how long its sweeps took:** `us` and `usx`, the shortest and
   the longest whole sweep among the sweeps that made the frame, in microseconds, **counting
   the plugin's calls alone** (the time inside `g_scanner.sweep()`, added over the sweep's
   segments; no arm, no settle, nothing between sweeps). `sps` stays what it is. With the
   points of the plan this is the time a point costs, which is what tells 1.6 from 0.94 at a
   narrow bandwidth, and its spread is what the *[later]* item on the latency (a reading
   that did not change costs three updates) looks like on the air.
4. **Every `scope` frame of a plan with more than one segment says what the chip's error
   word held:** `dev`, four hex digits, the OR over the frame's sweeps of `GetDeviceErrors`
   as read **after a sweep and before anything else is asked of the chip**.
   - The arm already goes to standby, which is where the word answers
     (`readDeviceErrors`): it is read there first, then cleared, then the arm goes on as
     today, so that the calibration's own verdict is still its own. Through `ScopeArmRadio`,
     host-tested, in `armScopeSegment`'s order.
   - What is read at the arm of segment *k* is what the sweep of the segment before it left
     (the last segment's for segment 0). A frame's `dev` covers its sweeps; nothing is
     carried from one frame to the next.
   - **A plan of one segment is not given a standby it does not have today**: its frames
     carry no `dev`. The words say so, and that such a span is asked for in two segments
     (`seg_mhz`) when the word is wanted.
   - Bits of interest, named in the words and in a test: `PLL_LOCK_ERR` (6), `PLL_CALIB_ERR`
     (2), `XOSC_START_ERR` (5), beside `IMG_CALIB_ERR` (4) that the arm already reads.
   - The read and the clear cost the arm time. Say how much by the code, and put it in the
     plan's time if it is more than a rounding (the `ack`'s `sweep` and the port's interval
     stay true).
5. **Nothing else of a frame moves.** `us`, `usx` and `dev` stand after `sps` and before
   `lv`; every other member is where and what it is. A frame grows by about forty bytes: the
   port's rule (a frame takes at most half the port's time) is worked out from the new
   length. Say the new bytes a frame for 250 and for 386 points in the report: the lists of
   the garden are sized from them (the Walter keeps 65 536 bytes a step).
6. **Host tests:** the words taken out in every position, alone and together, with and
   without the numbers; both of a pair refused; ten lines of today's, word for word what
   they parse to on `main`; the `ack`'s bytes with and without; the arm's calls in order
   with the read and the clear, for a first segment, a later one, a refused calibration and
   a reload; `us`/`usx` from a clock the test steps (the scope's seam gets one, as `HistRadio::nowUs`
   is the histogram's);
   `dev` gathered over a frame and dropped at the next; a one-segment plan's frame without
   `dev`; the radio as it was after such a scope; `tools/noisescope/scope_plans.json`
   unedited. Every line used as an example in a test is 40 characters or fewer, and a test
   says so.
7. **`tools/noisescope/echoes.py` and `waterfall_plot.py` read a capture with the new
   members and words** as they read one without (ignored where they have no use for them).
   What they do with them is task 04's.
8. **The words.** `docs/noisescope.md` §5: the four words in the command's row, the `ack`'s
   paragraph, `us`, `usx` and `dev` in the `scope` message with the error word's bits; §6: a
   paragraph on why a scope can be asked for on the other regulator and at the lower gain
   (in our words; nothing copied from the data sheet, sections named), and that a session's
   line is 40 characters. `tools/noisescope/lists/` is not touched: 04 writes the garden's
   lists. A line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] A `scope` without the four words plans, arms, acks and sweeps as on `main`, bar the one read and one clear at each arm (the tests that hold it, unedited where they are about the plan and the `ack`; the arm's order test brought up to date and named).
- [ ] The four words are taken wherever they stand, a pair is refused, and the radio is what it was when the scope ends (the tests' names).
- [ ] A frame carries `us` and `usx` for the plugin's calls alone, and `dev` when the plan has more than one segment (the tests' names; one frame of each kind printed in the report).
- [ ] The frame's new length for 250 and 386 points, and the port's interval worked out from it.
- [ ] Nothing that transmits and nothing that writes flash was added (`docs/noisescope.md`'s first lines: *Never transmits. RX only. Never writes flash.*); the report says how it checked.
- [ ] The report's *Not verified here* says what the first session on the unit has to show for each of deliverables 1, 3 and 4, in one line each.
- [ ] `tools\verify.ps1` green. CI green.

## What only the unit can show

All of it. The README's *The garden instead of the bench* has the sessions: the same wide
span on the DC-DC and on the LDO in one session, with the gain down once; what `us` reads at
4.8 kHz against 83 ms and 48 ms for 250 points; whether `dev` is ever anything but `0000`.
Thomas starts them from meshmaster.dk; I read the lines.

## Out of scope

Making the LDO or the lower gain the build's choice. The other modes. The plugin's words and
its latency table. meshmaster.dk and the Walter (a frame's new members are read by name
there and the lines are passed on whole). The garden's lists and what `echoes.py` makes of
the new members: 04. A release.
