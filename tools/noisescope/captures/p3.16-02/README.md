# p3.16-02 — the captures behind the table of the two numbers

**There are none.** Phase 3.16's task 02 gave `scope` a segment's width and a
settle and was to measure what each one leaves and costs on the bench board;
`python tools/bench/bench.py status` found *NOT PRESENT* (no port with the rig's
serial number `E926F6BEB407A412`), and `bench.local.json` still read
`"rf": "dummy-load"`, so the generator step of `tasks/phase-3.16/README.md`,
*Before the run*, had not been made either. The task's deliverable 6 is
therefore *Not verified here* (`tasks/phase-3.16/reports/02.md`) and this
directory waits for the run, as `p3.16-01/` does.

What belongs here, one row a capture, when it is run:

| File | The line sent | Carrier | Date |
|---|---|---|---|
| | | | |

The run that fills it, with the scanner flashed, `"rf": "antenna"` in
`bench.local.json` and the generator on 869.618 MHz at about −30 dBm into the
radio (`docs/bench.md` §9, *The fast sweep*, the image check):

    python tools/noisescope/hunt.py --list tools/noisescope/lists/p3.16-02.txt \
        --label p3.16-02 --carrier 869.618

Thirty-two lines, sixteen for each of the two spans of 23 November: the line as
it is asked today, four segment widths, six settles, and the pair the two
captures of 23 November point at with one value either side of each
(`tools/noisescope/lists/p3.16-02.txt` says what each answers). It writes
`out/bench/echoes/p3.16-02/` — one `NN-<name>.ndjson` a line and a `run.json`
with every scope of the run. Copy the `.ndjson` files here, add a row each
above, and keep `run.json` with the report rather than here: it is the tool's
reading of the captures and can be made again from them
(`python tools/noisescope/echoes.py <file> --carrier 869.618 --json`).

**Read a capture of a line with a width in it with its `ack`.** A frame carries
`f0`, `step`, `n` and `rbw` and no segment width, so `echoes.py` takes the width
from the `ack`'s `segw=` — which is why `hunt.py` writes the ack at the top of
every capture, and why a capture whose first line is not its ack is read as a
`scope` asked for with no width at all.
