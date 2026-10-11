# 02 — The release

**Depends on:** 01.
**Spec:** [`spec/FIRMWARE-MANIFEST.md`](../../spec/FIRMWARE-MANIFEST.md) §1–§3, §6; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-8, S-11; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §9.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Pushing a tag `vX.Y.Z` on a commit CI has passed makes a GitHub Release that a
person, MeshMaster's catalogue and the Walter can all use: the images, a
`manifest.json` that says what each is, the signature over it, and
`SHA256SUMS`. *Run workflow* rehearses the whole thing and publishes nothing.

## Read first

The agent firmware's `.github/workflows/release.yml` at
`51faafb427cba41b0de9983736b6621bc155f829` — the shape to keep: a `ci-green`
job that refuses a commit CI has not passed, the build, the publish, a dry run
that is the same jobs. And `spec/tools/manifestref.py`, which is the manifest
as code and which the tooling here **imports rather than re-implements**.

## Deliverables

- **`tools/release/build_release.py`** (Python, standard library plus what
  `manifestref.py` needs for signing). Given a version, a commit and the build
  directory of each environment, it writes into `out/release/`:
  - per image, named `twinoak-<role>-<board>-<band>-vX.Y.Z.<ext>`:
    the **raw application binary** for the Walter's SWD flasher (`use: swd`,
    with its `load_address`), the DFU package (`usb-dfu`) and the UF2
    (`usb-uf2`);
  - `manifest.json` in the canonical form, validated with
    `manifestref.validate` before it is written;
  - `SHA256SUMS` over every file including the manifest.

  The raw binary is made from the linked image, not assumed: its
  `load_address` is the lowest address the image occupies, it is contiguous,
  and a gap or a byte outside the application region is an error. The
  **application region** (`app_region.start`, `.end`) is read from the linker
  script the build actually used — the start of the application and the start
  of the file system above it — and never typed into the tool as a constant.
  The tool has tests: against the spec's fixtures for the manifest's shape, and
  against a small synthetic image for the binary, the gap and the
  out-of-region refusals.
- **The descriptor check is written and switched off.** `build_release.py
  --require-descriptor` looks for exactly one valid descriptor in each binary
  and holds it against the manifest entry (`manifestref.find_descriptor`).
  Images have none until phase 3's task 01, which turns the flag on in the
  workflow. Without the flag the tool says, per image, *no descriptor yet*.
- **Signing.**
  - `tools/release/sign_manifest.py` writes `manifest.json.sig` from a private
    key given in an environment variable, never as an argument and never
    echoed.
  - The workflow reads it from the repository secret `RELEASE_SIGNING_KEY`.
    **A tag with no such secret fails** — a release is signed or it is not a
    release — with a message that says how the key is made
    (`manifestref.py keygen`) and that the public half goes in
    `keys/release.pub`.
  - A dry run signs with the spec's **test key** and its version is
    `0.0.0-dryrun+<sha7>`, so the artifact can never be mistaken for a
    release. The tool refuses to sign a non-dry-run version with the test key,
    and a test holds that.
  - `keys/README.md`: what goes here, who makes it, and that the private half
    is never in the repository.
- **`.github/workflows/release.yml`**: on a tag `v[0-9]+.[0-9]+.[0-9]+` and on
  `workflow_dispatch` with a `ref` input. `ci-green` as the agent firmware has
  it, on `main`. Build every board environment with
  `-D TWINOAK_FW_VERSION="X.Y.Z+<sha7>"`. Run `build_release.py`, sign,
  verify the signature with the public key before anything is published, and
  then `gh release create` with every file — or, in a dry run, keep them as an
  artifact. Release notes say the version, the commit, the MeshCore tag and
  pin, which file is for what (the Walter; a laptop on the USB-C port), and
  that an image is only ever written to the application region.
- **`tools/verify.ps1`** gains a step: the release tooling's own tests, and a
  local dry run of `build_release.py` over the build it just made, with the
  manifest validated and signed with the test key and verified. So the release
  path is exercised on every commit without GitHub. Signing needs the
  `cryptography` package: pin its version in one place, and have the script get
  a Python that has it without installing anything into this machine's own
  (the container the native tests already use has `pip`).
- **`docs/release.md`**: how a release is made, what is in it, the key, and
  what to do when a tag is refused.

## Acceptance criteria

- [ ] `tools/verify.ps1` is green and now builds a dry-run release locally: `out/release/` holds the three files of the one image, `manifest.json`, its `.sig` and `SHA256SUMS`; `python spec/tools/manifestref.py verify <test public key> out/release/manifest.json` says *valid*.
- [ ] The manifest's `app_region` is what the linker script says, and the report shows both numbers and the lines of the script they came from.
- [ ] A *Run workflow* dry run on this task's commit produces the same set as an artifact; the report links the run.
- [ ] The tooling's tests cover: a gap in the image; a byte outside the region; signing a release version with the test key (refused); a missing key on the tag path (refused, with the instructions).
- [ ] Nothing prints a private key: grep the workflow log of the dry run for the test key's private hex and find nothing.

## Not verified here

- The tag path's `gh release create` — the same job as the dry run with one
  step more; pushing a tag would publish a release of upstream's stock
  repeater. **And it needs the release key, which Thomas makes**
  (`spec/SYSTEM.md` §10, item 1).
- That the raw binary boots when written over SWD: the Walter's flasher and
  the bench.

## Out of scope

The descriptor itself (phase 3, task 01). MeshMaster's catalogue
(`spec/proposals/02`).
