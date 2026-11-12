# 02 — The totals, told

**Depends on:** 01.
**Spec:** `docs/link.md` §2d (*The LPP channel table*: chosen once, never renumbered), `spec/LINK-PROTOCOL.md` §5.8 (`LPP_GET` answers the bytes a mesh telemetry request gets; read only); `src/common/power/Telemetry.h` and `.cpp` (`LppChannel`, `LppType`, `kMaxLppFields`, `ourFields`, `telemetryFields`, `encodeLpp`, `kMeshLppCap`), `BoardPower.cpp` (`appendOurFields`), `LinkPower.h`; CayenneLPP at the pinned version (`addGenericSensor`, `addField` and its type gate; the value is a `float`); `tools/linkcli.py` (`lpp`); `test/test_power`; 01's report.
**Size:** one session.
**Model:** claude-opus-5

## Goal

01 keeps the totals. This task reports them where the readings are already reported: in the
Cayenne-LPP a telemetry request is answered with, on the mesh and on the box link's `LPP_GET`, the
same bytes on both.

A reader takes two answers and subtracts. meshmaster.dk will; its pile is written from this task's
report, so the table below is a contract from the day this merges.

## Deliverables

1. **Nine new fields, type `genericSensor` (100: four bytes, unsigned), one channel each:**

   | LPP channel | What | Unit |
   |---:|---|---|
   | 6 | panel, charge | 0.01 mAh |
   | 7 | panel, energy | 0.1 mWh |
   | 8 | battery, charge in | 0.01 mAh |
   | 9 | battery, charge out | 0.01 mAh |
   | 10 | battery, energy in | 0.1 mWh |
   | 11 | battery, energy out | 0.1 mWh |
   | 12 | load, charge | 0.01 mAh |
   | 13 | load, energy | 0.1 mWh |
   | 14 | the seconds the totals cover | 1 s |

   The numbers are chosen here, once. If a unit turns out to be wrong for a reason below, change
   the unit and say why in the report; do not change a number.
2. **The units are checked, not taken.** An hour at 5 mA must be at least a hundred steps of
   each. The library takes a `float`: say what a total above 2^24 steps reads as, that it never
   reads lower than the one before, and after how long at 20 mA that starts. And the top: what a
   total does at the largest value four bytes hold is a decision, stated in `docs/link.md`, and
   never whatever a conversion happens to do.
3. **One list, two writers, the same bytes**, as today: `ourFields` and the mirror `encodeLpp`
   learn the type, and `appendOurFields` calls the library. The test that holds the two against
   each other covers the new fields, with a total above 2^24 among them.
4. **Behind the environment permission**, like channels 2 to 5. All nine or none: they are left
   out together until a first sweep has counted.
5. **What it costs.** The reply's bytes before and after against `kMeshLppCap`, and the airtime of
   a mesh telemetry reply before and after at SF8, 62.5 kHz, CR 4/8, in the report.
6. **`linkcli lpp`** names the nine and prints them in their units.
7. **The words.** `docs/link.md`: the table, the example, and one paragraph for whoever reads
   them: subtract two answers; seconds covered that went down mean the radio restarted and the
   count begins again; the difference in seconds covered against the clock says how much of the
   time was seen. `docs/ARCHITECTURE.md` §6. README's *The state of things*.

`POWER_GET` does not change: the specification is copied here and not edited (FOLLOWUPS).

## Acceptance criteria

- [ ] `LPP_GET` at the environment permission and a mesh telemetry reply carry the same bytes, with channels 6 to 14 after 5 (test). At base alone: channel 1 and nothing else (test).
- [ ] The mirror and the library write the same four bytes for a small total, for one above 2^24 and for the largest (test).
- [ ] Channels 1 to 5 are byte for byte what v0.1.2 answers for the same snapshot (test).
- [ ] The reply fits `kMeshLppCap` with every field present (test), and the report has the bytes and the airtime.
- [ ] `linkcli lpp` prints the nine with units (the tool's test).
- [ ] `tools/verify.ps1` green.

## Out of scope

meshmaster.dk and the Walter, which passes the bytes on as they are. The release (03). The board.
