# 01 — What flowed

**Depends on:** nothing.
**Spec:** `docs/ARCHITECTURE.md` §6; `src/common/power/PowerReader.h` and `.cpp` (the sweep: `kSweepIntervalMs`, one channel a pass, `dropDevice`), `Ina3221.h` and `.cpp` (`kConfigWord`: 64 averages at 1.1 ms, one set in about 422 ms; the shunts; `currentMilliAmps`, `busMilliVolts`), `BoardPower.cpp` (`loop`), `BatteryWatch.h` (what else reads channel 2); the INA3221 datasheet (averaging and conversion time; the conversion-ready flag of the Mask/Enable register); the design repository, read only: `hardware/pcb/CURRENT-SENSE.md` §3 and §6.7 at the commit `Ina3221.h` cites; `test/test_power`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

A reading of this board's power is the average of the last 0.42 seconds, taken once a second. Asked
once an hour, at the moment the Walter is awake to ask, it says 110 mA for a unit that draws a
fraction of that over the hour (README).

The reader already sees every second. This task makes it keep what it sees: per channel, how much
charge and how much energy has passed since the firmware started. Nothing goes on the wire here;
02 does that.

## Deliverables

1. **The totals**, in `src/common/power/`, host-portable like the rest of it:
   - **panel** (INA channel 1): charge and energy;
   - **battery** (channel 2): charge in, charge out, energy in, energy out. The sign of the current
     decides which, sweep by sweep;
   - **load** (channel 3): charge and energy;
   - **the seconds the totals cover**.

   Panel and load count what is positive. A negative reading there is noise around zero and adds
   nothing; say in the report what that costs at one step of noise.
2. **Added up from every completed sweep, with the time that really passed** since the sweep
   before, not an assumed second. Integers all the way, wide enough that nothing overflows or
   loses a step in years at full scale. Charge is current times time. Energy is the channel's own
   bus voltage times its current times time.
3. **A gap is a gap.** A sweep in which a channel did not read, the back-off after `dropDevice`,
   and a sweep that comes later than a bound you choose and state: that time is in no total and
   not in the seconds covered. A sweep counts for all three channels or for none.
4. **How much of the time a total really covers.** Today the device averages 0.42 s of each
   second. Decide from the datasheet how to cover most of it without a sweep ever reading a
   half-finished average: a longer average (§6.7 allows 64 to 256), the conversion-ready flag, or
   both. Say in the report what fraction is covered, and what a 100 ms burst of 1.5 A does to the
   hour's total in the best and the worst case.
   - The battery thresholds read channel 2 from the same cache. Show that they see what they saw.
   - The cost to `loop()` stays one channel a pass.
5. **Zero at start, and only then.** Kept in RAM. Nothing is written to flash.
6. **The words.** `docs/ARCHITECTURE.md` §6.

## Acceptance criteria

- [ ] A steady 100 mA on the load for a simulated hour is 100 mAh to within the rounding you state, and the seconds covered are 3600 (test).
- [ ] The battery at +500 mA for ten minutes and −50 mA for fifty: the two charges and the two energies are each right, and neither direction has the other's (test).
- [ ] A burst inside one sweep interval is counted for that interval and no other (test).
- [ ] A failed channel, the back-off and a late sweep each leave a gap: the totals do not move and the seconds covered do not count it (tests).
- [ ] A year at full scale on every channel overflows nothing (test, by arithmetic on the types).
- [ ] The battery thresholds' tests pass unchanged, or the report says which changed and why.
- [ ] No request handler performs I2C (the existing rail).
- [ ] `tools/verify.ps1` green.

## Out of scope

The wire (02). `POWER_GET` and the specification. The board.
