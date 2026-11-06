# Phase 3.8 — the radio says its band (link 1.1)

Written on the evening of 6 November. The first unit's Walter (v0.1.0) was asked for its state by
meshmaster.dk and could say its radio's role and board, never its band. The server picks a release's
image by role, board **and** band, so a radio with no golden image on its Walter could never be
offered one over LTE. The reviewer changed the specification the same evening: link **1.1** ends
`HELLO`'s answer and `EV_BOOT` with `band`. Nothing else in the link changed. This pile makes the
radio say it, and releases it.

The same evening found the Walter's faults (its pile 8) and meshmaster.dk's (pile 4.25). The radio
was the one part that behaved. It took a half-written application over SWD and came back intact
through its own USB-C and the release's `.uf2`, with its key, which is worth a line in the guide.

**Prerequisite:** link protocol 1.1 in `spec/`, the reviewer's copy of 6 November, committed with this
pile by the driver's notes commit:

- `spec/LINK-PROTOCOL.md` §5.1;
- `spec/fixtures/link/link-v1.1.json`, every case of 1.0 and the four of 1.1;
- `spec/fixtures/link/link-v1.json`, 1.0's, unchanged.

**Not on the board.** The unit on Thomas's bench is in the middle of things, so no session opens its
ports. Bench lines go into `docs/bench.md`.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [hello-says-its-band](01-hello-says-its-band.md) | — | `HELLO` answered in 1.1 to a 1.1 request, with the image's band at the end; `EV_BOOT` likewise; the descriptor and the release say link 1.1; tests against both fixture files |
| 02 | [the-release-0-1-1](02-the-release-0-1-1.md) | 01 | `v0.1.1` published as *Latest* and verified; the guide's recovery of a half-flashed radio |

```powershell
.\tasks\run-all.ps1 -Phase '3.8'
```

Run the Walter firmware's pile 8 and meshmaster.dk's pile 4.25 the same night. None waits for the others.

## Models

- **Opus**: 01, 02.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
