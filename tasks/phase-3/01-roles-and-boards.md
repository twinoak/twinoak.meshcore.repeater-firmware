# 01 — Roles and boards

**Depends on:** phase 2.
**Spec:** [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §2, §4, §5; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-11; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.1; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §1, §8, §9.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Every image carries, inside itself, what it is — role, board, band, version,
link version — so that the Walter can refuse to put the wrong thing on a radio,
and the source tree is arranged so that a second role and a second board are
additions, not rearrangements.

## Deliverables

- **The tree, by role.** `src/common/` for what every role on this board
  shares (the board target, the link codec and session, power and
  environment, the descriptor, the version) and `src/roles/repeater/` for what
  is the repeater's (`main.cpp`, `TwinOakMesh`, the clock keeper's wiring, the
  mesh client, the first-boot code). An environment compiles `common` and its
  own role and nothing of another's (`build_src_filter`). Moves only — no
  behaviour changes, and the native tests' file lists follow. If a file turns
  out to be half common and half the repeater's, split it and say so.
- **Names in one place.** `platformio.ini` gains, per environment, three
  values the build passes as defines and everything else reads from there:
  the **role** (`repeater`), the **board** (`rak4630-single`), the **band**
  (`868`). The environment is `<role>_<board with underscores>`. `HELLO`'s
  `role`, `board` and `slot` (0 for this board), the release tooling's image
  `id`, and the descriptor all come from those three — a test builds the
  expected strings from `platformio.ini` and holds each consumer against them.
- **The descriptor** (`src/common/fwdesc/`): the 96-byte structure of
  FIRMWARE-MANIFEST §4 as a `const` object the linker keeps and the firmware
  references (so it cannot be discarded), aligned to 4 bytes, with its CRC-32
  computed **at build time** — in the source if the language allows it at the
  board's standard, or by a post-link step that patches and re-checks; say
  which and why. A portable builder of the same 96 bytes with a native test
  against `spec/fixtures/manifest/index.json` (`descriptors[*].hex` from the
  fields).
- **The build proves it.** A post-build check, part of `tools/verify.ps1` for
  every board environment: the raw application binary holds **exactly one**
  valid descriptor, and its fields are the environment's.
  `spec/tools/manifestref.py descriptor <bin>` is the reference; use it.
- **The release requires it.** `--require-descriptor` is switched on in
  `release.yml` and in the local dry run (phase 0, task 02 wrote the flag).
  The manifest's image entry now carries `requires.link_major`.
- **The band is real.** The radio's build defaults (frequency, and anything
  else that differs by band) come from a per-band block, and a build whose
  band and default frequency disagree fails with a sentence. 433 has no block
  yet; the mechanism has a test with a made-up one.
- **`docs/ARCHITECTURE.md` §8** updated with the tree as it is now and a short
  *adding a role* / *adding a board* list: the files to add and the two tables
  to extend.

## Tests (native)

- The descriptor builder against the spec's fixtures, field for field and
  byte for byte, and its refusal of a version or board string that does not
  fit.
- The names: role, board, band and environment agree across `platformio.ini`,
  the `HELLO` response and the manifest entry.
- The band check.

## Acceptance criteria

- [ ] `tools/verify.ps1` green, including the descriptor check on the built image; the report shows `manifestref.py descriptor` on it.
- [ ] `git diff --stat` for the move shows renames, and flash and RAM use are unchanged to within the descriptor's 96 bytes (numbers in the report).
- [ ] A local dry-run release with `--require-descriptor` passes, and fails when the descriptor is removed from the link (show it, then restore).
- [ ] An image built with a deliberately wrong board name fails the post-build check.

## Not verified here

That the Walter reads the descriptor from the image it downloads — its
repository's tests, against the same fixtures.

## Out of scope

A second role (02). A second board: the dual adapter has no PCB. The bridge.
