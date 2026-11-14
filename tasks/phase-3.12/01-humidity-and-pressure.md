# 01 — Humidity and pressure

**Depends on:** nothing.
**Spec:** `src/common/power/Telemetry.cpp` (`kTypes` ~L48: the types this firmware writes; `ourFields` ~L81–100: the enclosure's temperature ~L97 and nothing else from it; the comment above `kTypes`) and `Telemetry.h` (the channels ~L46–62, the type numbers ~L68–71), `BoardPower.cpp` (the mesh path's `addVoltage`, `addCurrent`, `addTemperature`), `Environment.h` (`EnvReadings`: `valid` bit 1 humidity, bit 2 pressure; `rh_centi`, `pressure_pa`), `LinkPower.h`, `test/test_power`, `docs/link.md` (what the telemetry carries; `LPP_GET`), `tasks/phase-1/reports/06.md` (~L216: *CayenneLPP 1.6.1 cannot encode humidity or pressure*), `tasks/phase-3.10/reports/02.md` (the reply's bytes and airtime with the totals); the library and upstream as this build has them: `.pio/libdeps/repeater_rak4630_single/CayenneLPP/src/CayenneLPP.cpp` (`isType`, `addRelativeHumidity`, `addBarometricPressure`, `getTypeSize`, `getTypeMultiplier`) and `.pio/libdeps/repeater_rak4630_single/MeshCore/src/helpers/sensors/EnvironmentSensorManager.cpp` (~L285: upstream's own BME280).
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

The unit's BME280 is read whole: `ENV_GET` on the box link carries temperature, humidity and
pressure. The telemetry a mesh reader gets carries the temperature only. On 14 November
meshmaster.dk showed channel 5 of the unit's repeater with one reading, and Thomas asked where
the humidity was.

It was left out on purpose, for a reason that is not true. Report 06 of phase 1 says CayenneLPP
1.6.1's `isType()` does not list humidity, pressure or power, so that adding one writes nothing.
The library this firmware builds with lists all three. Nothing in the build defines a
`CAYENNE_DISABLE_*`. Upstream's own BME280 code calls `addRelativeHumidity` and
`addBarometricPressure`, and stock repeaters send `power` in their telemetry today.

## Deliverables

1. **The library first.** A host test encodes one humidity and one pressure with the library
   itself and reads the bytes back. If it shows the library does refuse one, stop for that type
   and say so in the report with the bytes. Do not write a type by hand around the library.
2. **Humidity and pressure on channel 5**, after the temperature, each when its valid bit is set:
   relative humidity (type 104, one byte, half a per cent) and barometric pressure (type 115, two
   bytes, a tenth of a hectopascal). The mesh reply and `LPP_GET` write the same bytes, as they do
   for everything else.
3. **The order is kept.** Channels 1 to 5 as they are, the two new readings after channel 5's
   temperature, the totals after them. Say the reply's new length (102 bytes today) and its
   airtime at SF8 and 62.5 kHz, and that it stays under the cap.
4. **The wrong sentence is corrected where it stands:** the comment in `Telemetry.cpp`,
   `docs/link.md`, and a dated note at the head of that section of report 06 saying what was
   misread. The report's own text stays.
5. **Power stays out.** meshmaster.dk computes it from the volts and amps it already has.
6. **The words.** `docs/link.md`'s table; a line in `release-notes/unreleased/`.

## Acceptance criteria

- [ ] The library encodes humidity and pressure, shown by a test that uses it (test).
- [ ] A snapshot with all three valid gives channel 5 temperature, humidity and pressure, each the value `ENV_GET` reports for the same snapshot to the type's resolution (tests).
- [ ] A snapshot with humidity invalid leaves it out and keeps the other two; the same for pressure (tests).
- [ ] The mesh reply and `LPP_GET` are byte for byte the same for the same snapshot (test).
- [ ] The reply's length with everything valid is the number the report names, under the cap (test).
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The box link and the Walter. meshmaster.dk and the exit-node firmware: both decode the two types
already. The board: never a chip erase, never a write outside the application region, and no
session opens a port.
