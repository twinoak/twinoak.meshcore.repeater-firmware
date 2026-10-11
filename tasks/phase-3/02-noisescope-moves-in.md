# 02 — NoiseScope moves in

**Depends on:** 01.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-11; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §9 ("Telling the firmware apart"); [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §8; the hardware repository at `b798aab043281fa4c9b17802a164fa94be12e206`: `software/firmware/noisescope-rak4630/` (all of it, `README.md` and `PROTOCOL.md` first).
**Size:** one session.
**Model:** claude-opus-5

## Goal

NoiseScope — the SX1262 spectrum and noise scanner that is flashed in place of
the repeater when a site has a noise problem — becomes the second role of this
family: built here, from the same board target, released beside the repeater,
carrying a descriptor that says `scanner`. It behaves exactly as it does today.

It moves because it is the same board, the same pins, the same sensors and the
same release pipeline, and because the Walter will swap the two images on a
radio and must be able to trust what each says it is.

## Deliverables

- **`src/roles/scanner/`**: NoiseScope's sources from the hardware
  repository at the commit above, read without touching that repository
  (RUNBOOK §0) and brought in with their history noted in a header comment —
  the scanner, its command parser, its NDJSON output, the watchdog. Its
  vendored `variants/` and `boards/` are *not* brought: this repository's board
  target and `boards/rak4631.json` serve both roles.
- **It uses the common code where the two had copies**: the INA3221 with
  per-channel shunts (`src/common/power/`, phase 1's task 06 — NoiseScope's
  own `telemetry.cpp` did the same arithmetic) and the image descriptor. Its
  radio bring-up stays its own: it drives the SX1262 directly through
  RadioLib for RSSI sweeps and the spectral-scan patch, and must keep the
  exact settings its README promises (the same RX path as the repeater:
  boosted gain, the TCXO, the RF switch, the current limit).
- **The environment `scanner_rak4630_single`**, role `scanner`, with the
  libraries it needs pinned exactly. RadioLib: NoiseScope pinned the commit
  MeshCore pins; the repeater environment may use a registry release. Two
  environments may differ here — say what each uses and why, and make sure one
  cannot leak into the other's build.
- **On the link it is what it is today**: NDJSON lines and text commands on
  `Serial1` at 115200, protocol 0.1, unchanged — LINK-PROTOCOL §9 says how the
  Walter tells it from a repeater. **No frames.** Its `boot` and `info` lines
  gain two fields, `role` (`scanner`) and `board`, and its `ver` is this
  repository's version.
- **It never transmits and never writes flash** — its README's two promises,
  which are what makes a scanner-and-back round trip safe for the repeater's
  identity. A rails-test line for each: no transmit call of RadioLib's and no
  file-system include anywhere under `src/roles/scanner/`.
- **`docs/noisescope.md`**: its `README.md` and `PROTOCOL.md`, merged and
  corrected for where it lives now. `tools/waterfall_plot.py` and its
  requirements come along under `tools/noisescope/`.
- **The release** builds both environments; the manifest lists two images
  (`repeater.rak4630-single.868`, `scanner.rak4630-single.868`), each with its
  three files.
- **Portable parts get native tests** where NoiseScope has logic that is not
  the radio: the command parser, the NDJSON writer (a line is valid JSON,
  numbers are finite), the sweep's point arithmetic.

## Acceptance criteria

- [ ] `tools/verify.ps1` green and now builds two board environments; flash and RAM of both in the report.
- [ ] Both images pass the descriptor check; the scanner's role byte is 3.
- [ ] A local dry-run release lists two images in `manifest.json`, and `manifestref.py validate` (through the tooling) accepts it.
- [ ] The repeater's image is bit-for-bit what it was before this task, apart from the build's own version string — or the report says exactly what changed and why.
- [ ] The two rails-test lines exist and each failed once on purpose.

## Not verified here (Thomas, with a board)

Add to `docs/bench.md`, "8. NoiseScope":

- Flash the scanner over USB; `info`, `stat`, `sweep` on the USB console and
  on `Serial1` behave as `docs/noisescope.md` says; `pwr` matches the
  repeater's `power` numbers for the same board.
- Flash the repeater back: same ID as before, same name, same neighbours
  list filling up. Nothing of the node's settings was lost.

## Follow-up for the reviewer (not for a session)

The hardware repository still holds the old copy. Removing it, and leaving a
pointer in its `PROJECT-KNOWLEDGE.md` §8, is done there by hand once this task
has landed — say so at the top of the report.

## Out of scope

Putting NoiseScope's output into link frames (a later minor version of the
link, if ever). The Walter's NoiseScope session.
