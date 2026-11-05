# 01 — The first release

**Depends on:** phase 3.6 merged (it is); the release key, which exists: key id `73efdd383a957596`, made by Thomas on 20 October, `RELEASE_SIGNING_KEY` set here and in the Walter firmware.
**First:** check that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If it has gone, write `logs/STOPPED-01.md` saying so, change nothing else, and end: the key is Thomas's, and a desk cannot make it.
**Spec:** `docs/release.md` (what a release carries: per image `.bin` for the Walter's SWD, `.zip` for USB DFU, `.uf2` for the drive; the base `.hex` for `rak provision`; the manifest and its signature; *Making one*, *A rehearsal*, the refusals), `keys/README.md`, `.github/workflows/release.yml` (`ci-green`, `build`, `publish`), `tools/release/` (`build_release.py`, `sign_manifest.py`), `spec/tools/manifestref.py verify`, `docs/provisioning.md` (how a board becomes a unit), `docs/noisescope.md` (the scanner role, in the same release), README.md (*State*, *Building it*); the Walter firmware's phase 7 task 01 (its `docs/flashing.md`, which points here); exit-node firmware piles 1.8/35 and 1.9/37 (how a release was tagged, checked and made *Latest* there).
**Size:** one session.
**Model:** claude-opus-5

## Goal

Thomas, 5 November: he powered up the first unit. Its repeater came up on the
mesh, its Walter did not, and *we're missing the firmware as releases on GitHub,
together with a small guide and release notes.* Nothing has ever been tagged
here. The radio in the unit runs whatever was put on it by hand.

## Deliverables

0. **`keys/release.pub` as the signer reads it.** Thomas committed the public
   half on 20 October (`82e2534`) in the Walter firmware's trust-table form:

   ```
   ed25519 73efdd383a957596 dad705e5…f1fe
   ```

   `tools/release/sign_manifest.py`'s `read_public_key` wants the first line
   of content to be the 64 hex characters alone. So a tag would fail at
   `publish`, after the builds, with *does not hold 64 hexadecimal characters*.
   Nothing caught it, because the dry run signs with the test key and the
   tests use a stand-in file. Fix it at the reader, not the file, so that both
   repositories read the same line:
   - `read_public_key` accepts `ed25519 <key id> <64 hex>` and the bare 64 hex;
   - it recomputes the key id from the key, and refuses a line whose id is
     wrong;
   - `keys/README.md` describes that one form;
   - `tools/verify.ps1` gains a check that reads the **real**
     `keys/release.pub` with the reader the tag path uses, so a malformed file
     fails on a commit and not on a tag.

   A test covers each form and a wrong id.

1. **A short guide**, `docs/flashing.md`, for a person with a unit on a desk:
   - **By cable:** the adapter's USB-C, with the `.uf2` (double-tap reset,
     copy onto the drive) or the `.zip` (`adafruit-nrfutil dfu serial …`).
     Which file is the repeater and which is the scanner.
   - **Through the Walter:** the `.bin` over SWD (`rak` on the Walter's
     console, or *Offer a release* on meshmaster.dk once the Walter checks
     in), and `rak provision` with the base `.hex` for a new radio only.
   - **Afterwards:** what to check — the first-boot profile ran once, the
     channel is the Danish one, the advert is heard, and the box link
     answers (`tools/linkcli.py`).
   - **The admin password:** the firmware gives a random one that nobody
     knows (README). How to set your own, at the USB port, before the unit
     goes out (`docs/provisioning.md`).

   One page. README's *Building it* points to it.
2. **Release notes a person reads.** The release body is a short hand-written
   part, then what the tooling generates:
   - what this release is (the first, and what has and has not been on a
     board, from README's *State*);
   - which MeshCore it is (the pin);
   - the link to `docs/flashing.md`;
   - which Walter release it goes with.

   Keep the hand-written part in the repository (`release-notes/vX.Y.Z.md`)
   and put it in front of the generated part in `release.yml`. A tag without
   that file is refused, as a tag without a key is.
3. **The release.**
   - Tag `v0.1.0` on `main` once this task's pull request has merged and CI
     is green on the merge.
   - Watch `release.yml` to the end. Make the release *Latest*, not a
     pre-release (Thomas's rule for his firmware while he is its only user).
   - Download it once and verify the manifest's signature against
     `keys/release.pub` (`spec/tools/manifestref.py verify`), and the files'
     hashes. Say in the report that they match.
4. **Words:** README's *State* says a release exists and names it.

## Acceptance criteria

- [ ] `read_public_key` reads the committed `keys/release.pub` and returns key id `73efdd383a957596`; a wrong id and a malformed line are refused (tests); `tools/verify.ps1` reads the real file.
- [ ] `docs/flashing.md` exists, one page, linked from README.
- [ ] `v0.1.0` is a published *Latest* GitHub Release:
  - the repeater and the scanner images, each as `.bin`, `.zip` and `.uf2`;
  - the base `.hex`;
  - the signed manifest;
  - a body that starts with the hand-written part.
- [ ] The downloaded release verifies against `keys/release.pub` (report).
- [ ] `tools/verify.ps1` green, CI green.

## Out of scope

The bench walk. Changing what the firmware does. The Walter firmware's
release (its own phase 7).
