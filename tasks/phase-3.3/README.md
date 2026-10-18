# Phase 3.3 — the band's ceiling, and a bench a session may use

Written on 18 October from the reviews of phase 3 and 3.2
([`../phase-3/FOLLOWUPS.md`](../phase-3/FOLLOWUPS.md),
[`../phase-3.2/FOLLOWUPS.md`](../phase-3.2/FOLLOWUPS.md)), both written the
same day, and Thomas's RAK4630 on the USB port of the machine the driver runs
on. Three tasks.

**Prerequisite:** phase 3.2 merged (it is: PR #22), and the two reviews on
`main` — the driver's notes commit carries them if they are still in the
working tree.

The review found one thing a unit gets wrong out of the box: the home
channel's band allows a 10 % duty cycle and upstream starts a repeater at
50 % (01). Everything else phase 3 built is right as far as a reading can
tell, and three of its decisions can only be proved on a board — the radio's
power pin, the scanner's round trip that must leave the node's identity in
place, and the first boot. 02 builds the one tool a session may touch a board
with, with the rails in the tool; 03 turns `docs/bench.md`'s mechanical lines
into walks and, if the board is there, walks them.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-bands-ceiling](01-the-bands-ceiling.md) | phase 3.2 | the duty cycle in `[band_868]` and the profile's second line built from it; the client's ceiling re-derived at 10 %; `linkloop` in the native step; *eight places*; the paging arithmetic independent of the tag |
| 02 | [a-bench-a-session-may-use](02-a-bench-a-session-may-use.md) | 01 | `tools/bench/`: the rig by serial number, DFU of our own images only with every refusal before a port opens, never on air with a repeater image, a lock, a console that never sends or prints a password, *restore*; RUNBOOK §2's rule |
| 03 | [the-bench-walks-itself](03-the-bench-walks-itself.md) | 02 | `bench walk` for §1, §8, §9, §10; result files with the blanks filled; with the board present, the lines ticked as *bench* and the board put back |

01 → 02 → 03.

```powershell
.\tasks\run-all.ps1 -Phase '3.3'
```

(The quotes: PowerShell reads a bare `3.3` as a number.)

## Models

- **Fable** — 02. The tool that writes to a board while nobody watches: what
  it refuses is the whole point of it.
- **Opus** — 01, 03.

## Before you start it (Thomas)

- `bench.local.json` at the repository root (02 commits the example), with
  the RAK's USB serial number, **`"rf": "dummy-load"`** if the dummy load is
  on the antenna port, and `"identity": "keep"` unless that module is a spare
  — `disposable` lets 03 erase it for §10. Without the file the pile still
  runs: 02 and 03 are complete and tested, and the walks wait for you.
- Leave the RAK on a repeater image. 03 leaves it on the repeater image of the
  commit it walked (or the one it found, if it was given that image), with
  the same identity, and says which in its last line.

## The exit criterion

`tools/verify.ps1` green with `tools/bench`'s guards tested; with the board
present, §1, §8 and the scanner round trip of §9 walked by a session and
ticked as *bench*, the radio's `rom` known, and the board back as it was.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
