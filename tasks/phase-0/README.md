# Phase 0 — the repository

Three tasks that turn a repository holding documents into one that builds, tests,
releases and keeps up with MeshCore. No firmware of ours is written here: at the
end of this phase `pio run` builds **upstream's stock repeater** for the RAK4630
from the pinned dependency, so that phase 1 starts from a green build and not
from a toolchain question.

The model is twinoak/meshmaster-agent-firmware, whose bootstrap did the same
thing for an ESP32 board. Its report —
[`tasks/phase-4.2/reports/65.md` in twinoak/meshmaster.dk](https://github.com/twinoak/meshmaster.dk/blob/01ac515d1d0115a1e5cd38563bb5da755519417f/tasks/phase-4.2/reports/65.md)
— lists what a consumer of the MeshCore package does not get for free, and is
worth reading before task 01 meets the same things on an nRF52.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-toolchain](01-the-toolchain.md) | nothing | upstream's repeater builds for the RAK4630 from the pin; native tests run; `tools/verify.ps1`; CI |
| 02 | [the-release](02-the-release.md) | 01 | a tag makes a GitHub Release with the images, a manifest, its signature and `SHA256SUMS`; a dry run proves it |
| 03 | [the-upstream-watch](03-the-upstream-watch.md) | 01 | a weekly look at MeshCore's `repeater-v*` tags that opens the bump as a pull request, and the sync phase for the bumps that need a session |

01 → 02 → 03.

```powershell
.\tasks\run-all.ps1 -Phase 0
```

## Models

- **Fable** — 01. Every later task inherits how MeshCore is consumed and what
  `tools/verify.ps1` runs.
- **Opus** — 02, 03.

## The exit criterion

`tools/verify.ps1` is green on this machine and in CI; a dry run of the release
produced a manifest that `spec/tools/manifestref.py` validates; the upstream
watch, rehearsed against an older tag, opened a pull request that CI built.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
