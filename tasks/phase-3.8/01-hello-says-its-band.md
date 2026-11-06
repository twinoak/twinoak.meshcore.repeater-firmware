# 01 — HELLO says its band

**Depends on:** nothing in this phase; the spec copy of 6 November on `main`.
**Spec:** `spec/LINK-PROTOCOL.md` **1.1** (the status lines at its top; §5.1, the `band` row, *The band, and why it is here*, *Versions*: *the highest minor both ends know*; `EV_BOOT` carries the body *as the RAK speaks it*), `spec/fixtures/link/link-v1.1.json`, `spec/fixtures/link/link-v1.json`, `spec/tools/linkref.py`; `spec/FIRMWARE-MANIFEST.md` (the descriptor: `band`, `link_major`, `link_minor`; what a release's `link` says); `src/common/link/codec/` (`Messages.h`, `Numbers.h`: `kProtoMinor`, `highestMinorBothKnow`), `src/common/link/session/Session.cpp` (`helloBody` about line 125, the `HELLO` case about line 151, `EV_BOOT` about line 680); `platformio.ini` (`TWINOAK_ROLE`, `TWINOAK_BOARD`, `TWINOAK_BAND`, about line 353: the three strings that name an image), `src/common/fwdesc/` (the descriptor and its `link_minor`); `tools/release/build_release.py` (`link_version`, which reads `link-v1.json`), `tools/linkcli.py` (`hello`), `tools/bench/` (what reads `HELLO`); `test/test_link_codec`, `test/test_link_session`, `test/test_clock` (which hold the link to `link-v1.json` by name); `docs/link.md`, `docs/bench.md`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The Walter can say what its radio is (role, board, band) only from a golden image's descriptor, or
from `HELLO`. `HELLO` has carried role and board since 1.0, but not the band. Link 1.1 adds it at the
end of the answer's body: the band the running image was built for, the same string as its
descriptor's (`TWINOAK_BAND`, `868`). A 1.0 Walter ignores it. The Walter's pile 8 task 05 asks for it.

## Deliverables

1. **1.1 on the radio.**
   - `kProtoMinor` becomes 1.
   - `HELLO` is answered at the highest minor both ends know, as the session already does, and a body
     at minor 1 ends with `band`, from `TWINOAK_BAND`.
   - A 1.0 request gets a 1.0 body, with no band. That is the fixture *hello response from a 1.0
     radio to a 1.1 request: no band* seen from the other side; write the test that holds it.
   - `EV_BOOT` sends the body at this firmware's own minor, band included, as §5.1 says.
2. **Every role.** Read which roles answer `HELLO` (the repeater; and the scanner?). Each one that
   does says its own `TWINOAK_BAND`. A role that does not speak the link is left as it is.
3. **Both fixture files.**
   - The codec and session tests hold every case of `link-v1.1.json`. Add the new file beside the
     old one in the existing tests, rather than replacing it.
   - Every case of `link-v1.json` still passes: a 1.0 Walter, the one on the bench, must still be
     answered right.
4. **The image and the release say 1.1.**
   - The descriptor's `link_minor` is 1.
   - `build_release.py`'s `link_version` reads 1.1. Its comment already says a constant of the
     firmware's own should be the source. If one exists now (`kProtoMinor`), read the version from
     the firmware rather than from a fixture file's name.
   - Read `FIRMWARE-MANIFEST.md` for what must agree between a radio image's link and the Walter's,
     and say in the report that a 0.1.0 Walter (link 1.0) still accepts and flashes a 1.1 image.
5. **The tools.**
   - `tools/linkcli.py hello` prints the band.
   - Whatever in `tools/bench/` records `hello` keeps it. Test it against the fakes.
6. **Docs.**
   - `docs/link.md`: 1.1, and the band.
   - `docs/bench.md`, for Thomas: `rak hello` on a 0.1.1 Walter says `band 868` with this image, and
     `linkcli.py hello` says the same.

## Acceptance criteria

- [ ] `HELLO` and `EV_BOOT` in 1.1 with the band from `TWINOAK_BAND`; a 1.0 request still gets 1.0 (test).
- [ ] Every case of both fixture files passes.
- [ ] The descriptor and the release manifest say link 1.1; a 1.0 Walter still accepts the image (from the spec, in the report).
- [ ] `linkcli.py` and the bench tool show the band; docs updated.
- [ ] `tools/verify.ps1` green.

## Out of scope

Status, neighbours and owner-info over the link: Thomas's decision (`FOLLOWUPS.md`). The Walter's side
(its pile 8).
