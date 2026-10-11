# Phase 1 — a repeater that is ours

Six tasks from "upstream's stock repeater builds" to a repeater that runs our
application on top of upstream's, talks to the box beside it, keeps a sensible
clock without an RTC, and reports the unit's power and environment correctly.
At the end of this phase a board flashed with this firmware is a full MeshCore
repeater on the Danish channel that a bench tool — and later the Walter — can
manage over the link.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [our-application](01-our-application.md) | phase 0 | **the design's proof**: our `main.cpp` and `TwinOakMesh` on upstream's unmodified repeater; our board target; no default password |
| 02 | [the-link-codec](02-the-link-codec.md) | 01 | CRC, COBS, packets and every message of protocol 1.0, against the spec's fixtures |
| 03 | [the-link-session](03-the-link-session.md) | 02 | the link on `Serial1`: `HELLO`, `PING`, `STATUS`, `IDENTITY`, `REBOOT`, `EV_BOOT`, the retry memory; a bench tool that speaks it |
| 04 | [the-clock](04-the-clock.md) | 03 | time that never runs backwards across a reset; `TIME_SET` / `TIME_GET`; the boot advert held until the clock is set |
| 05 | [local-management](05-local-management.md) | 03 | `CLI` over the link; our settings in `sensor get/set`; `SLEEP_NOTICE` and attention |
| 06 | [power-and-environment](06-power-and-environment.md) | 05 | INA3221 with the right shunt per channel, BME280; on the link, in mesh telemetry and as the battery voltage; battery attention |

01 → 02 → 03 → 04 → 05 → 06. 04 and 05 both build on 03 and touch different
files, but the driver runs them in this order and 06 needs both.

```powershell
.\tasks\run-all.ps1 -Phase 1
```

## Models

- **Fable** — 01. It decides whether the design of `docs/ARCHITECTURE.md` §2
  holds, and what happens to a board with no settings.
- **Opus** — 02 to 06.

## The exit criterion

A RAK4630 on the adapter, flashed over USB, that repeats on 869.618 MHz;
answers `HELLO` on `Serial1` with its version and capabilities; has its clock
set over the link and keeps it across a reset; runs a console command sent over
the link; and reports panel, battery and load currents that agree with a
multimeter in size and in sign. The last clause, and every other one that needs
a board, is Thomas's: each task ends with a *Not verified here* list and adds
it to `docs/bench.md`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
