# Phase 3.11 — a release from the button

Written on 13 November.

**What happened.** On the exit-node firmware Thomas pressed *Run workflow* on `release` and nothing
was published: in the firmware repositories a run by hand is a dry run, and a release is a pushed
tag with its notes written first. On meshmaster.dk the same button deploys. Every release here so
far has been a task's work: `release-notes/vX.Y.Z.md` written, the tag pushed, the files checked.

**Decided, 13 November (Thomas):** the firmware repositories release the way meshmaster.dk does.

- **The button publishes.** Nothing is typed. A *dry run* box is there for a rehearsal.
- **The notes write themselves.** A task that changes something a person will notice leaves one
  line, as meshmaster.dk's tasks do. A release is those lines.
- **The version is in the repository.** It names the release that comes next. After a release the
  workflow moves it to the next patch, in a small pull request of its own. A bigger step is one
  line edited beforehand.
- **It refuses when there is nothing to say:** no line since the last release, no release.

A release is still signed or it is not a release, and still refused on a commit CI has not passed.
The exit-node firmware and the Walter firmware get the same, each in its own pile.

**Not on the board.** No session opens a port.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-release-from-the-button](01-a-release-from-the-button.md) | — | *Run workflow* publishes the version the repository names, signed, with notes compiled from the tasks' own lines, and then moves the version on; a dry run by a tick; the tag trigger gone |

```powershell
.\tasks\run-all.ps1 -Phase '3.11'
```

## Models

- **Opus**: 01.

## Afterwards (Thomas)

- Nothing to press yet: the only line waiting is this task's own. The first real release from the
  button is the next pile's, and it is the first time the publish and the pull request after it
  run for real. If that pull request does not land, the run says what to do; the release itself is
  out and is not undone.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
