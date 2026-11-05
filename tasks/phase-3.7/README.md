# Phase 3.7 — the first release

Written on 5 November. Thomas powered up the first unit: its repeater came up
on the mesh, and its Walter did not. There is nothing released to flash, no
guide, and no release notes. One task.

**Prerequisite:** the release key, which Thomas made on 20 October (key id
`73efdd383a957596`; `RELEASE_SIGNING_KEY` set here and in the Walter firmware).
Nothing to do. One thing found on the way: `keys/release.pub` is in the Walter
firmware's `ed25519 <key id> <hex>` form, which this repository's signer does not
read yet. A tag would have failed at `publish`. Task 01 fixes the reader first.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-first-release](01-the-first-release.md) | — | `keys/release.pub` read in the shared form (and checked on every commit), `docs/flashing.md`, a hand-written top to the release notes, and `v0.1.0` published as *Latest*, verified against it |

```powershell
.\tasks\run-all.ps1 -Phase '3.7'
```

## Models

- **Opus** — 01.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
