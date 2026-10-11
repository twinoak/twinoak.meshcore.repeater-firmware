# 06 — Power and environment

**Depends on:** 05.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.8, §8; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §3, §6; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `hardware/pcb/CURRENT-SENSE.md`, `PROJECT-KNOWLEDGE.md` §4–§5, and NoiseScope's `src/telemetry.cpp` and `src/config.h`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The unit knows its own power: what the panel gives, what the battery takes or
gives, what the load draws — each with **its own shunt** — and the temperature,
humidity and pressure inside the enclosure. Four readers get the same numbers:
the link, a mesh telemetry request, the battery voltage a `status` reports, and
the thresholds that wake the Walter before the battery is empty.

Stock MeshCore applies one shunt value, 0.1 Ω, to all three INA3221 channels.
On this hardware that is right for one channel and wrong by a factor of five
and two for the others; it is why the unit runs custom firmware at all.

## The hardware, from the documents above

| Channel | Shunt | Measures | Sign of current |
|---|---|---|---|
| 1 | 100 mΩ (RS2) | panel, bus voltage at the charger's input | positive = into the charger |
| 2 | 20 mΩ (RS3) | battery, bus voltage at the battery | **positive = charging, negative = discharging** |
| 3 | 50 mΩ (RS4) | total load, bus voltage after the shunt | positive = drawn |

INA3221 at `0x42`, BME280 at `0x76`, one bus, 1 k pull-ups through a 1000 pF
filter: **100 kHz or slower**. The INA3221 is on the B-board and stays powered
when the LoRa rail is killed; the BME280 is on the A-board.

## Deliverables

- **`src/power/`**:
  - the conversion from the INA3221's raw registers to millivolts and
    milliamps — register layout, the 40 µV and 8 mV steps, the per-channel
    shunt, the sign — as portable functions with the shunts in one table
    (build-time constants; the table is the board's);
  - a reader that samples all three channels on a schedule (not on demand: a
    request must not wait on I2C), keeps the last good value and a validity
    bit per channel, and gives up on a transaction that takes too long — a
    sensor that stopped answering is a missing reading, never a stalled loop;
  - the BME280, read so that the link's numbers and the mesh's agree: either
    through upstream's manager or beside it, and the report says which and
    why.
- **The link**: `POWER_GET`, `ENV_GET`, `LPP_GET` (§5.8) and the capability
  bits `POWER` and `ENV`. `LPP_GET` returns exactly what a mesh telemetry
  request at those permissions gets.
- **Mesh telemetry**: `TwinOakSensors::querySensors` adds, after whatever
  upstream's manager adds, the three channels' voltage and current in
  Cayenne LPP. Choose the channel numbers once, write them in `docs/link.md`
  as a table (MeshMaster will name them), and never renumber them.
  Upstream's own INA3221 support stays out of the build
  (`ENV_INCLUDE_INA3221` undefined) — a rails-test line holds that.
- **The battery voltage**: the board's `getBattMilliVolts()` returns channel
  2's bus voltage (0 when the channel is invalid), so `status`, the telemetry
  base channel and upstream's own power management see the real battery and
  not an unconnected ADC pin. Read what upstream's nRF52 power management does
  with that number at boot and at low voltage (`NRF52_POWER_MANAGEMENT`), and
  say in the report whether it can shut this node down and at what voltage —
  that is a decision for the reviewer, not one to take silently.
- **Battery attention** (§8, reasons 2 and 3): *low* and *critical* thresholds
  on channel 2's bus voltage with hysteresis and a minimum time below — a
  transmit burst must not raise it — through task 05's attention call, under
  its hourly limit. Defaults **3500 mV** and **3300 mV**, placeholders until
  the bench has a discharge curve; settable and kept across a restart through
  two more settings, `power.low_mv` and `power.crit_mv`, in a small
  preferences file of our own (versioned; not upstream's).
- **`linkcli.py`**: `power`, `env`, `lpp [mask]`.

## Tests (native)

- The conversion against hand-calculated register values for each channel:
  full scale, zero, one LSB, a negative battery current; and against the
  figures of `CURRENT-SENSE.md` (0.4, 2 and 0.8 mA per LSB).
- The reader against a fake bus: a channel that fails goes invalid and comes
  back; a stuck transaction does not hold the schedule.
- The thresholds: a dip shorter than the minimum time raises nothing; low,
  then critical; recovery needs the hysteresis; nothing re-raised inside the
  hour.
- The fixture cases of `POWER_GET` and `ENV_GET` through the session, byte for
  byte, including the negative temperature.
- The LPP of `LPP_GET` equals the LPP the telemetry path builds, for the same
  fake readings.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The LPP channel table is in `docs/link.md`.
- [ ] The report answers the power-management question above with the upstream lines it read.
- [ ] A request on the link never performs I2C: show it from the code's structure.

## Not verified here (Thomas, with the boards)

Add to `docs/bench.md`, "5. Power" — the hardware repository's
`CURRENT-SENSE.md` §6 is the fuller procedure; these are the firmware's lines:

- `linkcli.py power` against a multimeter: panel current with the bench supply
  as the panel; battery current positive while charging and negative with the
  panel off; load current equal to the two radios' draw. Sign first, then
  size.
- The same three numbers in a phone's telemetry view of the repeater, and the
  battery voltage in its status.
- `linkcli.py env` against a thermometer.
- With the battery simulator stepped below 3500 mV for a minute: an attention
  reason 2; below 3300 mV: reason 3.
- With the INA3221's bus unplugged: the repeater keeps repeating, `power`
  answers with `valid` 0.

## Out of scope

Trending, alerts and energy balance (MeshMaster). Charger state (the BQ24650
has no digital interface). The RTC.
