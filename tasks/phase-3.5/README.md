# Phase 3.5 — what the first board showed, and MeshCore 1.18

Written on 22 October, the evening the first board was walked (phase 3.4's task 02), from what that
walk found, from the decisions phase 3's review had left to Thomas — which he took the same evening —
and from MeshCore's `repeater-v1.18.0`, which the weekly workflow found on 19 October. Six tasks for
one night. The board is on this machine's USB the whole time, on its dummy load, with a disposable
identity, so every task that can show its work on the board does.

**Prerequisites:** phase 3.4 merged (it is: PR #28). The reviewer's notes in the working tree — this
pile, and the boxes ticked in the follow-ups of phases 3, 3.1, 3.2 and 3.4 — land with the driver's
pre-run notes commit.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [a-new-unit-on-the-bench](01-a-new-unit-on-the-bench.md) | phase 3.4 | a radio that arrives with only its base is flashed **and restored** without a hand; `--tick` says *already ticked* when it is |
| 02 | [the-scope-across-the-band](02-the-scope-across-the-band.md) | 01 | the call that fails at the third segment named in the `err` line; `scope 150 960` streaming on the board, or exactly why not |
| 03 | [a-watchdog-an-update-gets-through](03-a-watchdog-an-update-gets-through.md) | 02 | the scanner's hardware watchdog at a timeout a measured USB update fits in, the 8 s loop check in software; a scanner replaced in **one** DFU |
| 04 | [the-clock-and-the-thresholds-walked](04-the-clock-and-the-thresholds-walked.md) | 03 | `bench walk 3` and `bench walk 5`: the USB console's lines of *The clock* and *Power*, on a board with no RTC fitted |
| 05 | [the-danish-standard](05-the-danish-standard.md) | 04 | the four settings Thomas chose in the first-boot profile, the advert interval derived from the node's key; walk 10 on the board |
| 06 | [meshcore-1-18](06-meshcore-1-18.md) | 05 | the first real bump: `repeater-v1.18.0` read at the eight places, checked for protocol changes, every walk of the night repeated at the new pin, merged — or its report on `main` saying why not |

01 → 02 → 03 → 04 → 05 → 06.

```powershell
.\tasks\run-all.ps1 -Phase '3.5'
```

**Every task lands even when its finding is not fixed.** Each task's *Deliverables* say what lands
when the fix is out of reach — the evidence, the narrowest statement of the fault, a follow-up for
whoever must take it next — so that one hard radio problem does not end the night. The driver stops
only where a task does not land, and that should mean a session that could not run, not a finding.

## Models

- **Fable** — 02 and 06. A radio fault that shows only on the board; and 266 commits of somebody
  else's to read for what they change in ours.
- **Opus** — 01, 03, 04, 05.

## Before you start it (Thomas)

- `bench.local.json` as phase 3.4 left it: serial `E926F6BEB407A412`, `"rf": "dummy-load"` — **and the
  dummy load really on the antenna port** — and `"identity": "disposable"`.
- **The LTE box stays unplugged** from connector B's J1 all night (phase 3.4's README: the Walter's
  supervisor must not act on a radio a walk is flashing).
- The RAK adapter's USB-C on this machine and the lab supply on. Nothing else holds the radio's COM
  ports: no serial monitor, no browser tab.
- Not at the same time as the Walter's phase 6 (the same lock per radio). The Walter's phase 4 may run
  beside it: it touches no board.
- Two drivers on one account share its usage limit. A session that hits it is retried by the driver,
  and after too many the run stops with *resume with -From NN* — nothing is lost.

## The exit criterion

`tools/verify.ps1` green at the pin the night ends on. On the board: a new unit's first flash
restorable by the tool; `scope 150 960` streaming, or the failing call named; a scanner replaced by
one DFU; *The clock* and *Power* walked; a fresh unit's profile carrying the Danish standard; and
MeshCore 1.18.0 walked — merged, or its report on `main` saying why not. The board left on this
repository's repeater image.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
