# Phase 3.10 — what flowed, not what flows

Written on 12 November, after reading a week of the first unit's telemetry on meshmaster.dk.

**What was seen.** Every one of the unit's last 20 hourly readings says its load draws 109 to 118 mA,
with peaks of 147 to 171. The battery channel says the same with the sign turned. All of them were asked for by
the unit's own Walter, so with the Walter awake and its modem up. There is no reading from while
it sleeps, which is nearly all of the time.

**Why.** A telemetry request is answered with the INA3221's last sweep: the average of the 0.42
seconds before it. That is a true reading of a moment, and the only moment anybody asks is the
worst one. meshmaster.dk draws those moments as if they were the hour.

**What this pile does.** The radio never sleeps, and it already reads the three channels once a
second. It adds up what has flowed since it started and reports the totals beside the readings. A
reader takes the difference between two answers and has what really flowed between them, whenever
it asked. meshmaster.dk's half follows in its own pile, after this one's reports.

**Decided, 12 November (Thomas):** totals at the source. Not a reading saved from while the Walter
sleeps, which would hide what the Walter itself costs.

**What it is not.** A fuel gauge. The battery channel resolves 2 mA
(`hardware/pcb/CURRENT-SENSE.md` §3 in the design repository: *too coarse for coulomb counting*).
The totals are good for a day's energy and an hour's average current. State of charge stays a
matter of voltage.

**Not on the board.** The radio is in the unit. No session opens its port.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [what-flowed](01-what-flowed.md) | — | charge and energy per INA3221 channel since start, the battery's by direction, and the seconds they cover; added up from every sweep with the time that really passed; nothing on the wire yet |
| 02 | [the-totals-told](02-the-totals-told.md) | 01 | LPP channels 6 to 14 carry the totals, on the mesh and on `LPP_GET` byte for byte; the table in `docs/link.md`; `linkcli` names them |
| 03 | [the-release-0-1-3](03-the-release-0-1-3.md) | 01, 02 | `v0.1.3` published as *Latest* and verified |

```powershell
.\tasks\run-all.ps1 -Phase '3.10'
```

## Models

- **Opus**: 01, 02, 03.

## Afterwards (Thomas)

- meshmaster.dk: import `v0.1.3`, canary for LTE Exit Node 1, then *Offer a release* and *Apply
  the candidate* on the unit's page for the **Repeater** product.
- Until meshmaster.dk's own pile lands, the new channels show there as *Channel 6 genericSensor*
  and so on: nine numbers that only grow. That is them working.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
