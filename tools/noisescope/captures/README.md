# Captures kept with the tools that read them

NoiseScope NDJSON, as a unit or a bench board said it, kept here when something
in the repository is held to it. Everything a run leaves lives in `out/bench/`
and is git-ignored; a file is copied here only when a test or a report reads it.

`python tools/noisescope/echoes.py <file> --carrier <MHz>` reads any of them
(`docs/noisescope.md` §7); `python tools/noisescope/waterfall_plot.py --file
<file>` draws one. A **NoiseScope session's** download is read as a session
instead — `echoes.py --steps <file> --carrier <MHz>`, one table across its
steps and what every pair of them says — and goes in `garden/`.

| File | What it is |
|---|---|
| `bellevue-2026-11-23.ndjson` | **Not in this checkout.** The lines of NoiseScope session 3 (Bellevue, 23 November 2026, meshmaster.dk `/noisescope/3`, *Download the lines*) — the session the echoes of a wide `scope` were first read in. `tools/noisescope/test_echoes.py` holds `echoes.py` to the table of `tasks/phase-3.16/README.md` for its wide `scope` **when the file is here**, and skips that one test when it is not. Saving it here is all it takes. |
| `p3.16-01/` | **Empty of captures.** Phase 3.16's task 01 was to run `tools/noisescope/hunt.py` on the bench board and keep what came back; no board was on the bench that day (`tasks/phase-3.16/reports/01.md`, *Not verified here*), so there was nothing to keep. Its own `README.md` says what goes in it. |
| `p3.16-02/` | **Empty of captures**, for the same reason and the same day: phase 3.16's task 02 gave `scope` a segment's width and a settle and had no board to measure them on (`tasks/phase-3.16/reports/02.md`). Its `README.md` carries the one command that fills it and the list of lines it runs. |
| `garden/` | **Empty of captures.** Where a NoiseScope session's *Download the lines* goes, under its own name and unedited: the echoes are measured on the unit at Bellevue and not on a bench (`tasks/phase-3.16/README.md`, *The garden instead of the bench*). Its `README.md` has the one command over such a file and what the four sessions are. |

A capture is the board's own bytes and not a re-serialisation of them, except
for a `scope`'s `ack`: `hunt.py` writes that back as JSON at the top of each
file, because the plan a board *took* is worth keeping beside the frames and
the ack is read before the collecting starts (`tools/bench/console.py`,
`collect_raw`).
