# 02 — The first board

**Depends on:** 01; a RAK4630 on the production adapter with its base (the Walter's `rak provision`, the hardware repository's bench tool, or a probe) and on this machine's USB; `bench.local.json`; the LTE box unplugged from connector B's J1 for the run (the phase README: the Walter's supervisor must not act on the radio while a walk flashes it).
**Spec:** `docs/bench.md` §0a–§0b (what `bench walk` runs and what stays a person's), §1, §8, §9, §10; `tools/bench/` (phase 3.3, tasks 02–03: `bench.py`, `walk.py`, the lock); `tasks/RUNBOOK.md` §2 (a session touches a board only through `tools/bench/`); `tasks/phase-3/FOLLOWUPS.md` (the three `[3.3]` items: `radio_ok` with the power pin high, the scanner round trip keeping the identity, a power cut during the first boot) and `tasks/phase-3.1/FOLLOWUPS.md` (the fast sweep's bench walk, gated on `rom` = `SX1261 V2D 2D02`); `docs/provisioning.md` §1 (what a blank radio does on its first boot).
**Size:** one session, long.
**Model:** claude-opus-5

## Goal

Phase 3.3 built the walks and had no board to walk them on: every report since phase 0 ends with
*no board, no COM port*. Now there is one, on the production RAK adapter, with a bootloader under it.
This task walks what a session may walk, ticks what agrees, fixes what does not when the fix is small,
and leaves the board exactly as it found it.

## Deliverables

- **`bench status`** first: the board found by its USB serial number (the FICR device id — say whether
  the bootloader's port and the application's report the same one; `bootloader_serial` exists for the
  case they do not), the lock free, what runs on it. A radio with a base but no application (fresh
  from `rak provision`) is a valid start: the first `bench flash` gives it this commit's repeater
  image, and its first boot is §10's *First boot* — say so in the result file.
- **`bench walk 1`**, **`bench walk 8`** (the descriptor in the file, the flash, `hello` over the link
  if the dongle is configured, `fw.version` — the three agreeing) and **`bench walk 9`** (the scanner's
  round trip: `boot`, `radio_ok`, `info`, `stat`, the sweep, `hist`, `dwell`, `cad`, `stop`, a minute
  with no `nan`, `rom`, `scope 863 870`, the dummy-load baseline, **and the repeater back with its
  identity**). With `"identity": "disposable"` also **`bench walk 10`** (the first boot, `erase`, the
  provisioning at the USB port and over the link). Each with `--tick`.
- **The `[3.3]` items of phase 3's follow-ups**, from the walks' results: `radio_ok` with the power pin
  high; the identity across the scanner round trip; the power cut during a first boot is a person's —
  write it as *needs a person* with the exact moment.
- **The fast sweep** (phase 3.1's follow-up): if `rom` reads `SX1261 V2D 2D02`, its walk lines too; if
  not, the `rom` that was read, in the follow-up, and the walk not attempted.
- **Fixes**: what a walk shows broken in code this repository owns is fixed in this task when it is
  small — a native test that reproduces it first, then the fix, then the walk again; larger, a
  follow-up for the task that wrote the line, with the result file named.
- **At the end** `bench restore`: the role and the identity found at the start (or, for a radio that
  arrived with no application, this commit's repeater image and the identity its first boot made),
  said in the last line.

## Acceptance criteria

- [ ] §1, §8 and §9 walked on the board and ticked *bench* or followed up; §10 too if the identity was disposable.
- [ ] The two USB serial numbers (bootloader's, application's) recorded in `docs/bench.md`'s §0a table.
- [ ] `rom` recorded; the fast sweep walked or the reason it was not.
- [ ] The board restored, its identity as found (the tool's last line, in the report).
- [ ] `tools/verify.ps1` green.

## Not verified here (Thomas)

Everything in `docs/bench.md` §0b's right-hand column: the adverts and the repeating on the air, a
phone's login, the scope on SCL, the power cut, the Walter's half of §8.

## Out of scope

SWD (the Walter's). A release with the real key (Thomas, `software/FIRST-UNIT.md` in the hardware
repository).
