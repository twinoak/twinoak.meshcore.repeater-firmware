# p3.16-01 — the captures behind the tables

**There are none.** Phase 3.16's task 01 asked the bench board where the echoes
of a wide `scope` are; `python tools/bench/bench.py status` found *NOT PRESENT*
(no port with the rig's serial number), and the generator step of
`tasks/phase-3.16/README.md`, *Before the run*, had not been made either —
`bench.local.json` still read `"rf": "dummy-load"`. So the task's deliverable 6
is *Not verified here* (`tasks/phase-3.16/reports/01.md`) and this directory
waits for the run.

What belongs here, one row a capture, when it is run:

| File | The line sent | Carrier | Date |
|---|---|---|---|
| | | | |

The run that fills it, with the scanner flashed and the generator on
869.618 MHz at about −30 dBm into the radio (`docs/bench.md` §9, *The fast
sweep*, the image check):

    python tools/noisescope/hunt.py --label bellevue-carrier --carrier 869.618

It writes `out/bench/echoes/bellevue-carrier/` — one `NN-<name>.ndjson` a line
and a `run.json` with every scope of the run. Copy the `.ndjson` files here, add
a row each above, and keep `run.json` with the report rather than here: it is
the tool's reading of the captures and can be made again from them
(`python tools/noisescope/echoes.py <file> --carrier 869.618 --json`).
