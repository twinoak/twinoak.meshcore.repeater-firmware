# 04 — The clock and the thresholds, walked

**Depends on:** 03.
**Spec:** `docs/bench.md` §0b (what a walk is; what stays a person's), §3 *The clock* and §5 *Power*; `docs/link.md` §2a (`time_state`, and when a backwards set is accepted); `tools/bench/walk.py` (`SECTIONS`, `PageLine`, the four existing walks, `tick_page`), `tools/bench/console.py` (what it refuses, and why), `tools/bench/fakes.py`; `src/roles/repeater/clock/ClockKeeper.h/.cpp` (the keeper, `clkreboot`), the console's `sensor get clock.state`; the thresholds `power.low_mv` / `power.crit_mv` (where they are kept, their limits, the refusal); `tasks/phase-1/reports/04.md` and `06.md` (what the clock and power lines were written from); `tasks/phase-3.3/reports/03.md` (how a walk is written, and the rules it keeps).
**Size:** one session.
**Model:** claude-opus-5

## Goal

*The clock* and *Power* are the first unit's time and battery, and neither has run on a board.

**The clock matters more than usual on this batch,** because the first boards are built without the
RV-3028: U5 is not fitted (the hardware repository's `PROJECT-KNOWLEDGE.md` §1). What keeps a
repeater's adverts in time across a restart is therefore the clock keeper, together with the Walter
setting the time after every boot.

**The thresholds** are what raise `EV_ATTENTION` before the battery is flat.

Most of their lines need the link, a phone, a meter or the lab supply's knob. The ones the USB console
can do become two more walks.

## Deliverables

1. **`bench walk 3`: the console's lines of §3.**
   - After a fresh flash of this commit's repeater image (`rf: dummy-load` holds, as for every repeater
     flash):
     - `sensor get clock.state` says `unset`;
     - upstream's own time readout shows its built-in date in May 2024.
   - **Setting the time:** upstream's `time <epoch>`, using this machine's time, moves `clock.state` to
     `set` within one pass of `loop()`. Upstream's `clock sync`, if the pin has it, does the same.
   - **After a `reboot`:** `clock.state` is `restored`, and the clock is a few seconds on from the set.
     Not 2024, and not an hour on.
   - **`clkreboot`:** the boot after it says `restored`, ahead of where the set left it and never behind.
     That is the keeper's whole point.
   - **No RTC fitted:** `clock.state` never says `rtc` on this board. If it does, that is a finding.
   - **Needs a person**, with the reason for each in the result file:
     - the `linkcli.py` lines;
     - the reset switch;
     - power off for a minute;
     - a companion's view of the adverts;
     - a hand-fitted RV-3028.
2. **`bench walk 5`: the console's lines of §5.**
   - `sensor set power.low_mv 3600` answers `ok`, `sensor list 5` shows it, and after a `reboot` it is
     still 3600.
   - `3350`, `2400` and `4400` are refused, with the reply the pinned firmware actually gives. Read it;
     don't copy it from the page.
   - **Put the value back:** the walk records `power.low_mv` and `power.crit_mv` at its start and leaves
     both as it found them. Its last line says so.
   - The DMM, phone, thermometer, simulator and unplugged-bus lines need a person.
3. **Both walks follow p3.3-03's rules:**
   - every step prints what it sent and what it saw, then *ok*, *differs* or *needs a person*;
   - a result file under `out/bench/`, with the page's `→ ______` blanks filled where a value was read;
   - then `--tick`.

   A *differs* is a follow-up for the task that wrote the line (p1-04, p1-06), never a fix here. The one
   exception is a bug in the walk itself.
4. **Host tests against the fakes for both walks.** The fake board grows:
   - a clock that a `time` set moves;
   - a keeper state that a `reboot` restores;
   - the two thresholds with their refusals.
5. **Docs:** `docs/bench.md` §0b's table gets §3 and §5 in the walk column, and their lines say what
   the walk does and what stays yours. If `console.py` refuses a line these walks need (`clkreboot`?),
   say why the refusal can be lifted for that line, or why it cannot and the line stays a person's.

## Acceptance criteria

- [ ] `bench walk 3` and `bench walk 5` exist, are host-tested, and were walked on the board, with result files in the report; their lines are ticked *bench* or followed up.
- [ ] `power.low_mv` and `power.crit_mv` back at their found values (the walk's last line), and the identity unchanged.
- [ ] `tools/verify.ps1` green.

## Out of scope

Anything that needs the link (a dongle or the Walter), the RV-3028, or the lab supply's knob.
