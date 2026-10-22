# 03 — A watchdog an update gets through

**Depends on:** 02.
**Spec:** `src/roles/scanner/Wdt.h` (`CONFIG.HALT = Pause`, `CONFIG.SLEEP = Run`, `CRV` from `WDT_TIMEOUT_S`), `src/roles/scanner/config.h` (~L116, `WDT_TIMEOUT_S 8`), `src/roles/scanner/main.cpp` (`wdtBegin` ~L407, `wdtFeed` ~L419, the reset reason ~L126); `docs/noisescope.md` §5 (`boot` and its `reset`, `reboot`, `dfu`, *Forwarding it, without understanding it*) and §6 (*Watchdog*); the nRF52840's WDT as the MDK on disk describes it (`nrf52840_bitfields.h`: CONFIG, CRV, RREN, RR; and its product specification's WDT chapter if it can be fetched: the watchdog cannot be stopped, and which resets clear it); [`../phase-3/FOLLOWUPS.md`](../phase-3/FOLLOWUPS.md), *The scanner's watchdog ends the DFU that would replace it* (this task); `out/bench/flash.log` (every DFU of 22 October with its `what` and `result`) and `tasks/phase-3.4/reports/02.md`; `tools/bench/flash.py` (the two-DFU recovery 3.4-02 added).
**Size:** one session.
**Model:** claude-opus-5

## Decision (Thomas, 22 October)

The hardware watchdog stays, at a timeout a USB update measurably fits in. The 8 s loop check that
NoiseScope was designed around (*a step that takes longer than the timeout is a step that should not
have been started*) moves into software. The alternatives were to leave the two DFUs as they are, or
to use only a software watchdog. Neither is this task's.

## Goal

NoiseScope starts the nRF52840's watchdog at 8 s. It cannot be stopped, and it survives the soft
reset into the bootloader with the same timeout. The bootloader does not feed it. So a USB update over
a running scanner dies about eight seconds in, and the radio comes back in its bootloader with no
application. The Walter's SWD updates are not affected, because `HALT = Pause` stops the watchdog
while a debugger holds the core. Keep it that way.

## Deliverables

1. **Measure on the board** with `tools/bench`:
   - What to time: from the start of a DFU to the application's port coming back, for the largest image
     the scanner is replaced by (the repeater, about 414 KB).
   - Where from: the bootloader, where no watchdog runs, so this is the update's own duration.
   - How often: three runs.

   Record every time in the report.
2. **The hardware timeout:** `WDT_TIMEOUT_S` = the longest measured update × 2, rounded up to whole
   tens of seconds. A comment at the define points to the measurement in the report. Nothing else in
   `Wdt.h` changes; `HALT = Pause` in particular stays, and a test holds it (a grep is enough).
3. **The 8 s loop check, in software:**
   - **The trigger:** the main loop leaves a heartbeat, checked from an interrupt that does not depend on
     the loop. Use a timer or RTC compare the scanner does not already use, and check that it doesn't.
     If the loop has not come round in 8 s, reset.
   - **What `boot` reports:** `boot`'s `reset` says `wdt` after it, because both watchdogs mean *the
     loop stopped*. If the two are told apart, it is in a field §5's NDJSON allows to be added, not in a
     new `reset` word that a reader enumerating them would not know.
   - **How the reset is marked:** not with `GPREGRET`, which belongs to the bootloader. Use a `.noinit`
     marker, or whatever fits once `RESETREAS` has been read.
   - **The split:** the deadline logic is host-portable and native-tested; the board half is thin.
4. **A scanner replaced in one DFU.** On the board, scanner → repeater with `bench flash`;
   `out/bench/flash.log` must show **one** DFU, `ok`. Then repeater → scanner → scanner, then back to
   the repeater. The bench tool's two-DFU recovery stays, because a scanner image from before this task
   still has the 8 s watchdog. Its message says which case it was.
5. **If a command to provoke a hang is added,** so that 9.6 (*a watchdog reset, and `reset` reading
   `wdt` after it*) becomes walkable:
   - it is the scanner's only, never the repeater's;
   - it is documented in `docs/noisescope.md` §5;
   - it is harmless when a Walter forwards it as text.

   Walk 9.6 on the board with it and tick it. Without such a command, 9.6 stays a person's.
6. **Docs:** `docs/noisescope.md` §6 *Watchdog* for the two halves; `docs/bench.md` §9 and §0a for
   what a person should expect now from a scanner's update.

## Acceptance criteria

- [ ] The measured update times are in the report, and `WDT_TIMEOUT_S` is derived from them.
- [ ] `HALT = Pause` is unchanged (test).
- [ ] The software check's deadline logic is native-tested; if a hang can be provoked, on the board it ends in a reset within about 9 s that `boot` reports as `wdt`.
- [ ] On the board, a scanner is replaced by the repeater in one DFU (`flash.log`), and the board is restored with its identity.
- [ ] `tools/verify.ps1` green.

## Out of scope

A watchdog for the repeater role: upstream's loop has none, and adding one to the node that must
repeat for months is a decision of its own (`Wdt.h`'s comment). The scope's segments (02).
