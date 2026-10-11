# Phase 3 — the family

Three tasks that turn one firmware into a family: every image says what it is,
a second role moves in beside the repeater, and a board out of the box comes up
the way a TwinOak unit should.

More roles and boards are coming — a bridge between two bands, the dual
adapter's two radio positions (`docs/ARCHITECTURE.md` §8). None of them is
built here. What is built is the structure they will fit without another
reorganisation: a role is a directory and an environment, a board is a target
and a table row, and the Walter can tell them apart before it flashes anything.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [roles-and-boards](01-roles-and-boards.md) | phase 2 | `src/` split into what is common and what is a role's; the descriptor in every image; the release checks it |
| 02 | [noisescope-moves-in](02-noisescope-moves-in.md) | 01 | NoiseScope builds here as the role `scanner`, from the same board target and power code, in the same release |
| 03 | [the-first-boot-profile](03-the-first-boot-profile.md) | 01 | what a unit is before anybody has provisioned it, and the two ways to provision it |

01 → 02 → 03.

```powershell
.\tasks\run-all.ps1 -Phase 3
```

## Models

**Opus** for all three.

## The exit criterion

A release dry run produces two images — `repeater` and `scanner` for
`rak4630-single`, band `868` — each with exactly one descriptor that agrees
with the manifest, and `spec/tools/manifestref.py descriptor` reads both.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
