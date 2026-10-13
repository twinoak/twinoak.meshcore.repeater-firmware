# Phase 3.1 — the fast sweep

One task, written on 13 October from MeshCore issue
[#3578](https://github.com/meshcore-dev/MeshCore/issues/3578) and one message
of Thomas's: *"This illustrates more or less what I'd like NoiseScope to be
able to do … as wide as the radio allows it, but having the span
configurable. The waterfall below is awesome."*

**Prerequisite:** phase 3 merged (it is: PR #19). Phase 3's own review
(`../phase-3/FOLLOWUPS.md`) has not been written yet; if it tags anything for
the scanner, it goes in before this runs.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-fast-sweep](01-the-fast-sweep.md) | phase 3 | `scope`: the in-radio sweep of #3578 as a streaming mode of the scanner, its span configurable up to 150–960 MHz in calibrated segments, frames sized for each sink, and a live spectrum-over-waterfall in the plotter |

```powershell
.\tasks\run-all.ps1 -Phase '3.1'
```

(The quotes: PowerShell reads a bare `3.1` as a number.)

## Models

**Opus** for 01.

## The exit criterion

`scope 150 960 2048` is planned, validated and framed by native-tested code;
both images build; the bench walk in `docs/bench.md` is written for Thomas.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
