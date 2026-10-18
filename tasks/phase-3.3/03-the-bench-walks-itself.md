# 03 — The bench walks itself

**Depends on:** 02.
**Spec:** [`docs/bench.md`](../../docs/bench.md) §1, §8, §9 (with *The fast sweep*) and §10, line by line; [`docs/noisescope.md`](../../docs/noisescope.md) (the lines a scanner writes, `info`'s `rom`, §6 *the fast sweep*); [`docs/provisioning.md`](../../docs/provisioning.md) §3; `tools/bench/` (02) and the rule it put in `tasks/RUNBOOK.md` §2; `tools/noisescope/waterfall_plot.py` (`--save`, `--baseline`); the phase-3 review ([`../phase-3/FOLLOWUPS.md`](../phase-3/FOLLOWUPS.md)) and the 3.1 review ([`../phase-3.1/FOLLOWUPS.md`](../phase-3.1/FOLLOWUPS.md): the six lines of the fast sweep, the first of which gates the rest).
**Size:** one session, long.
**Model:** claude-opus-5

## Goal

Turn the lines of `docs/bench.md` a script can check into walks
`tools/bench` runs, and — if the RAK is on this machine — run them, tick what
they confirmed and put back what they found. The lines that need a person's
hand (a power cut, a phone, the roof, a generator, a scope probe) stay a
person's, and the page says which those are.

## Deliverables

- **`bench walk <section>`** for §1, §8, §9 and §10, each a list of steps
  that are the page's lines in the page's order, each step printing the line
  it checks, what it saw and *ok*, *differs* or *needs a person*. The result
  is a Markdown file under `out/bench/` named by date and commit, with every
  `→ ______` of the walked lines filled in. Nothing that is a password is in
  it (02's console guarantees it; test the file anyway).
- **§1, the repeater on USB:** `Repeater ID:` (64 hex), `ver`, `board`,
  `get radio`, `sensor list` (`10 vars`, the first five, the cut digit),
  `sensor list 5`, `sensor get fw.version`, `sensor set fw.version x`
  refused, and 01's `get dutycycle` on a unit that has had its first boot
  since 01.
- **§8, what an image says it is:** the descriptor of the file, the flash,
  then `hello` over the link (if the dongle is there) and the console's
  `fw.version` — the three must agree.
- **§9, NoiseScope, as a round trip:** record the repeater's identity;
  flash the scanner; the `boot` line's fields and **`radio_ok`**; `info`'s
  **`rom`** — if it is not `SX1261 V2D 2D02`, the fast-sweep steps are
  skipped with that reason and the walk continues; a `sweep` of the page's
  281 points and its time; `hist`, a short `dwell`, `cad`, `stop` in the
  middle of a sweep; every line of a minute's output parses and none carries
  `nan`; if the rom is right, `scope 863 870` (the `ack`'s fields and
  `sps`) and, **only with the RF port on the dummy load**, the baseline
  `scope 150 960 2048` saved with `--save` beside its NDJSON in `out/bench/`;
  then flash the repeater back and check the same `Repeater ID`, name and
  `fw.version`. The watchdog, the image check with a generator, the site
  through the filter and the Walter UART line are a person's.
- **§10, only with `identity: disposable` in the bench config:** `erase`,
  reset, the new `Repeater ID:`, both `First boot:` lines,
  `firstboot.state` → `done`, `path.hash.mode` → `1`, the duty cycle → `10`,
  `get guest.password` empty; `reboot` and the second boot's `never`. Then a
  provisioning file through `linkcli.py provision` (if the dongle is there)
  or the console, with a generated admin password the tool never prints and
  hands to nobody — the unit is a bench unit. The power cut stays a person's.
- **If the board is on this machine, walk them** (§1, §8, §9; §10 only if
  disposable), tick each confirmed line in `docs/bench.md` as
  `[x] … (bench, <date>, <commit>)`, move each *differs* to the follow-ups of
  the task that wrote the line, and put the result file's content in the
  report. If it is not: say so; the walks are still complete and tested
  against 02's fakes.
- `docs/bench.md`'s head says which lines `bench walk` covers and which stay
  a person's, in one table.

## Acceptance criteria

- [ ] Each walk runs against 02's fake serial layer in a host test, through to its result file, and a password never reaches the file.
- [ ] With the board present: the walks' result files in the report, the lines ticked with *bench*, and the board back to the repeater image and identity it started with (02's `restore`, last line of the transcript).
- [ ] Without the board: *no bench* in the report, and every walk's host test green.
- [ ] `tools/verify.ps1` green.

## Not verified here

Whatever the walks marked *needs a person* — and, if the rom is not the
plugin's, the fast sweep on this part.

## Out of scope

Changing the firmware because a walk found something: a *differs* is a
follow-up, not a fix in this task.
