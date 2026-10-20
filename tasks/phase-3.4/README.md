# Phase 3.4 — the base, and the first board

Written on 20 October, the day the first unit was powered and its RAK4630 turned out to carry RAK's
RUI3 instead of the bootloader and SoftDevice this firmware is linked against. Since that day the
Walter gives a factory radio its base, by a person's command (`spec/SYSTEM.md` **S-5a**), from a file
that **this repository's release carries**: `use: provision`, Intel HEX, one per board
(`spec/FIRMWARE-MANIFEST.md` §2, §4a). Two tasks: the base in the release; and the first walk of this
firmware on the real board, now that it can have a bootloader.

**Prerequisites:** phase 3.3 merged (it is: PR #25); `spec/` copied from the hardware repository at a
commit that has S-5a and §4a (`python spec/tools/specsync.py verify spec` passes, and
`spec/tools/manifestref.py` has `validate_provision`).

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-base-in-the-release](01-the-base-in-the-release.md) | phase 3.3; the spec copy | RAK's RAK4630 bootloader + S140 6.1.1, vendored at a pinned source and hash, held to §4a in CI; a `provision` file in every `rak4630-*` image of the manifest; the release refuses one without it |
| 02 | [the-first-board](02-the-first-board.md) | 01; a radio with its base | `bench walk 1`, `8`, `9` (and `10` with a disposable radio) on the production RAK adapter; what differs fixed or followed up; the board left as found |

01 → 02.

```powershell
.\tasks\run-all.ps1 -Phase '3.4'
```

## Models

- **Fable** — 01. A binary from outside the repository that ends up in every radio: where it came
  from and what it may contain is the task.
- **Opus** — 02.

## Before you start it (Thomas)

- For 02, the radio needs its base. Either the Walter has given it (its phase 6, task 03), or you
  did it with the hardware repository's `software/firmware/walter-swd-bench`, or with a probe. A
  radio still on RUI3 makes 02 stop at its first line (it cannot be found by its USB serial number:
  RUI3 enumerates differently).
- `bench.local.json` as phase 3.3 made it: the RAK's USB serial number (after the base, the
  bootloader and the application report the FICR device id), `"rf": "dummy-load"` for the repeater
  walks, `"identity": "keep"` unless this module is a spare.
- **Not at the same time as the Walter's bench** (its phase 6): the two take the same lock per radio,
  and a held lock stops a task.
- **The Walter must leave the radio alone during 02.** The lock lives on this machine; the Walter in
  the unit does not know it, and its supervisor may reset, power-cycle or reflash the radio over SWD
  while a walk is flashing it over USB. Unplug the LTE box from connector B's J1 for the run (the
  radio's power comes from connector A, not from the Walter: the LoRa rail stays on with the kill line
  floating), and plug it back after.

## The exit criterion

A tagged release of this repository would carry, in every `rak4630-single` image, a `provision` file
that `manifestref.validate_provision` accepts against the image's region, whose source and hash are
written down and checked in CI. And on the board: §1 and §8 walked and ticked as *bench*, the
scanner's round trip of §9 with the identity kept, the radio's `rom` known.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
