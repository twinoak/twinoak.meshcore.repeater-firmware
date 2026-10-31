# Phase 3.6 — built at home

Written on 31 October from Thomas's note of that evening: all CI is to run on the Unraid runners,
the organisation's self-hosted GitHub Actions runners (`myoung34/github-runner`, label `unraid`),
because GitHub-hosted minutes got out of hand. meshmaster.dk moved on 29 October; this repository
still runs every job on `ubuntu-latest`. One task.

**Prerequisites:** phase 3.5 merged (it is: PR #34).

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [built-at-home](01-built-at-home.md) | phase 3.5 | `ci.yml` and `upstream.yml` on `[self-hosted, unraid]`, their caches only on GitHub's runners; `release.yml`, which builds and signs what ships, stays on GitHub's runners; a guard in `ci.yml` that holds both |

```powershell
.\tasks\run-all.ps1 -Phase '3.6'
```

## Models

- **Opus** — 01.

## Before you start it (Thomas)

Nothing on the bench: this pile touches no board, so it may run beside any other pile. Two drivers on
one account share its usage limit.

## The exit criterion

`tools/verify.ps1` green; this pile's own pull request checked entirely on the Unraid runners;
`release.yml` untouched but for its comment, on GitHub's runners.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
