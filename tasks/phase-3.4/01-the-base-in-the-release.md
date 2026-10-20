# 01 — The base in the release

**Depends on:** phase 3.3; `spec/` copied with S-5a and §4a.
**Spec:** [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §2 (`use: provision`, `format` `hex`, no `load_address`, at most one per image), §4a (the seven rules), §5 (5p: the Walter judges the base against *its* board region), §6 (`manifest-repeater-base.json`, `base-*.hex`); `spec/tools/manifestref.py` `validate`, `validate_provision`, `base` (the command line); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-5a, S-8, §10.2; `boards/rak4631.json` (SoftDevice S140 6.1.1, `settings_addr` `0xFF000`), the linker script `nrf52840_s140_v6.ld` (the application region the release already reads), `tools/release/{build_release,selfcheck,sign_manifest,test_release}.py`, `docs/release.md`, `.github/workflows/release.yml`, `docs/ARCHITECTURE.md` (the flash map, §~698–705); RAKwireless' `RAKWireless/WisBlock` repository, `bootloader/RAK4630/` (read with `gh api`, public), and `adafruit/Adafruit_nRF52_Bootloader` (its releases and `src/boards/` — the second source).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

Every radio this repository's images go onto needs, once, the MBR, SoftDevice S140 6.1.1 and the
bootloader underneath them. The Walter writes that base (S-5a) from the release, so the release
must carry it — and because it is a binary nobody here built, the task is mostly about *which* file,
from *where*, proven to be *what*: a vendored file at a pinned source, its hash written down, held to
§4a on every run, and refused by the release if it is missing or has changed.

## Deliverables

- **The file**, vendored under `boards/base/rak4630/`:
  - Two candidates, judged by the same three questions: RAK's current combined RAK4630 bootloader +
    S140 6.1.1 Intel HEX in `RAKWireless/WisBlock` `bootloader/RAK4630/`, and Adafruit's release for
    the **RAK4631** board (`Adafruit_nRF52_Bootloader`, a tagged release asset built for that board).
    The questions: does it pass §4a (it must *include* the SoftDevice — a bootloader-only file is what
    the first board's reports warned against, and §4a refuses it)? Is it built for the RAK4631 (its
    board directory, its pins)? **Does its source tree use Nordic's MDK 8.40.2 or later** — the one
    that copies `UICR.APPROTECT` into `APPROTECT.DISABLE` at boot? A bootloader built before that
    closes a revision 3 radio's debug port at every reset, which takes away S-5's whole SWD update path
    and the update over LTE (`spec/SYSTEM.md` §10.2). Prefer, in that order: passes §4a, built for the
    RAK4631, MDK 8.40.2 or later. Pin the chosen file by commit and path (or release tag and asset),
    and say in the report how each candidate answered each question.
  - **Stop** (`STOPPED-01.md`) rather than vendor a bootloader built for another board, and rather
    than guess the MDK: if neither candidate is built for the RAK4631 and passes §4a, or the MDK cannot
    be told for the one that does, the report says what was found and Thomas decides. Never a file
    from a forum, a mirror or a branch tip.
  - `boards/base/rak4630/README.md`: the source (repository, commit, path, the release tag if any),
    the bootloader's version as the file and its source say it, the SoftDevice's, the SHA-256 of the
    file exactly as downloaded, what `manifestref.py base` says of it against the board's region, and
    **what is known about the bootloader's handling of APPROTECT on a revision 3 part** — whether its
    source tree's MDK is 8.40.2 or later (it then copies `UICR.APPROTECT` into `APPROTECT.DISABLE`
    and the Walter can reach the radio after the reset; `spec/SYSTEM.md` §10.2). If that cannot be
    told from the source, write *unknown, the first board says* — not a guess.
  - The file's licences (Adafruit's bootloader is MIT; Nordic's SoftDevice has its own licence for
    redistribution in binary form with Nordic devices): name them, link them, and add the notices
    the licences ask for to the release (`THIRD-PARTY-NOTICES` or the repository's equivalent).
  - The file is committed **byte for byte as downloaded** (`.gitattributes`: binary, no line-ending
    conversion — an Intel HEX file's hash changes with its line endings, and the hash is the pin).
- **The pin, held in CI** (`tools/release/selfcheck.py` or a new `tools/base/check.py` that it calls):
  the vendored file's SHA-256 equals the README's; `manifestref.validate_provision` accepts it against
  the region the release already reads from the linker script; the summary it returns (bootloader
  address, SoftDevice) equals the README's. `tools/verify.ps1` runs it, so a changed file fails
  locally first.
- **The release** (`tools/release/build_release.py`): every image whose `board` is `rak4630-*` gets
  one file `use: provision`, `format: hex`, named
  `twinoak-base-rak4630-<bootloader version>-s140-6.1.1.hex`, the same file under every such image
  (one hash). The manifest still passes `manifestref.validate` (which now knows the use). The release
  **refuses** — before signing — a manifest with a `rak4630-*` image without its base, or with a base
  whose check fails. `docs/release.md`: one section, *The base*, with the table of files a release now
  carries.
- **Tests** (`tools/release/test_release.py`): the dry run's manifest has the base in every
  `rak4630-*` image and validates; a changed byte in the vendored file fails the check with the
  hash; a bootloader-only file (the spec's `base-bootloader-only.hex`) put in its place is refused with
  §4a's words; a manifest built without the base is refused before signing.

## Acceptance criteria

- [ ] The vendored file, its source pinned by commit and path, its hash and its §4a summary in `boards/base/rak4630/README.md`; the APPROTECT question answered from the source or marked *unknown, the first board says*.
- [ ] The release's dry run carries the base in every `rak4630-single` image and `manifestref.validate` passes on it.
- [ ] The release refuses a missing or changed base before signing (tests).
- [ ] Licences named and their notices in the release.
- [ ] `tools/verify.ps1` green.

## Not verified here

Whether the radio boots from this base: the Walter's phase 6 task 03 (S-5a on silicon), then this
pile's 02.

## Out of scope

Writing it to a radio (the Walter). A bootloader of our own build. The bridge and dual boards (their
`board` names get the same base when they exist; nothing to do now).
