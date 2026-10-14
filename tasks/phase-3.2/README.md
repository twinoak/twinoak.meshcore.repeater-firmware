# Phase 3.2 — a bump can be green

One task, written on 14 October as the reviewer's answer to the one open item
of phase 0: Thomas wants a MeshCore release that changes nothing we rely on to
arrive as a green pull request a person merges, and two tests made every bump
red by naming the current tag and pin.

**Prerequisite:** phase 3.1 merged (it is: PR #20).

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [a-bump-can-be-green](01-a-bump-can-be-green.md) | phase 3.1 | the version tests assert the defines, not a release; the release tooling's synthetic build info names no release; `docs/upstream.md` says tier one is real; a rehearsal bump is green |

```powershell
.\tasks\run-all.ps1 -Phase '3.2'
```

(The quotes: PowerShell reads a bare `3.2` as a number.)

## Models

**Opus** for 01.

## The exit criterion

The rehearsal bump's pull request is green with no session involved, and the
phase-0 follow-up is ticked.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
