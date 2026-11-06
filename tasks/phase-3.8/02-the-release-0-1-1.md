# 02 — The release 0.1.1

**Depends on:** 01 merged.
**First:** check that `keys/release.pub` holds the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-02.md` saying which, change nothing else, and end.
**Spec:** `docs/release.md`, `keys/`, `.github/workflows/release.yml`, `tools/release/`, `tasks/phase-3.7/01-the-first-release.md` and its report (how v0.1.0 was tagged, checked and made *Latest*); `docs/flashing.md`, README.md (*The state of things*), `docs/bench.md`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

01 makes the radio say its band. A radio has to run it before the Walter can report the band without a
golden image, and before meshmaster.dk can offer a radio its first image over LTE. Publish it.

## Deliverables

1. **The release notes' hand-written top:** link 1.1, and the band in `HELLO`, with why (one
   sentence). A 0.1.0 Walter still speaks to it.
2. **`docs/flashing.md`, *A radio left half-written*.** On 6 November a Walter at 0.1.0 started a
   first install, was reset by its watchdog part-way through, and left the radio's application
   half-written. The radio came back over its own USB-C:
   - double-tap reset; if the application is gone, the bootloader may already be up;
   - copy the release's `.uf2` onto the drive;
   - the radio boots this release with its key and settings, because neither the Walter's flasher nor
     the `.uf2` writes outside the application region.

   Check each sentence against the code and the bootloader before writing it. Then give the one line
   that shows it worked: `rak hello` on the Walter, or `linkcli.py hello`.
3. **README's *The state of things*:** current.
4. **The tag.**
   - Tag `v0.1.1` as `docs/release.md` says.
   - CI is green and the release is published as **Latest**.
   - The files verify against the shared key, as downloaded from the release.
   - The report has the URL, the files with their hashes, and the verifier's last line.

## Acceptance criteria

- [ ] `v0.1.1` is published as *Latest* and every file verifies (in the report).
- [ ] The notes' top and *A radio left half-written* are written and checked against the code.
- [ ] README's state of things is current.

## Out of scope

The board. The Walter's release (its pile 8).
