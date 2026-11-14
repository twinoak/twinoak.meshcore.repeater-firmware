# Phase 3.12 — the humidity that was there all along

Written on 14 November.

**What happened.** Thomas looked at the unit's repeater on meshmaster.dk and missed the
humidity. The BME280 is read whole and the box link carries all three of its readings; the
telemetry reply carries the temperature alone. It was left out in phase 1 because the report of
its task 06 says the Cayenne LPP library cannot encode humidity or pressure. The library can.

**And one from 13 November.** The first release from the button published v0.1.4 and left a red
run, a pull request waiting at *verify — Expected*, and a second red run an hour later. Nothing
was wrong; nothing said so.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [humidity-and-pressure](01-humidity-and-pressure.md) | — | channel 5 of the telemetry carries the enclosure's humidity and pressure beside its temperature, in the mesh reply and in `LPP_GET` alike; the wrong sentence about the library corrected where it stands |
| 02 | [the-release-says-what-is-left](02-the-release-says-what-is-left.md) | 01 | a release that published is a green run that names the one pull request left to merge, and that pull request says what is true about its check |

```powershell
.\tasks\run-all.ps1 -Phase '3.12'
```

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **Release** with the button, merge its notes pull request, then offer and apply the radio's
  firmware to the unit from meshmaster.dk. Channel 5 then shows three readings.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
