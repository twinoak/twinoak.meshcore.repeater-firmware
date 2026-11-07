# Phase 3.8 — follow-ups

Items for a later task of this phase are tagged with it. The review of 01–02 goes below them when it
is written.

- [x] **[done by p3.9-02, 7 November: link 1.2's `LOCAL_REQUEST` implemented, with upstream's own
  `handleRequest` and upstream unedited] Status, neighbours and owner-info of the unit's own repeater, through its Walter.**
  6 November: the Walter's *local door* (its `components/agent/include/walter_agent/link_mesh.h`)
  serves a job for the unit's own repeater without the air. The login is answered locally, a CLI line
  is the link's `CLI`, telemetry is `LPP_GET`. Status, neighbours, owner-info, discovery and trace have
  no request on the link, so they are *Failed*: the unit cannot read its own repeater's status at all,
  and meshmaster.dk's scheduled `status` and `neighbours` of that repeater fail through its uplink.
  Two ways, one decision:
  - a new link request (1.2) that answers what MeshCore's `REQ_TYPE_GET_STATUS` and the neighbours
    request would, as bytes;
  - the CLI's own stats lines (`stats-core`, `stats-radio`, `stats-packets` at the pin) through `CLI`,
    with the Walter shaping them into what agent-core expects.

  The first is exact and is a spec change. The second needs none, but it parses text. meshmaster.dk's
  pile 4.25 task 169 routes around it meanwhile.

  **Settled 7 November (Thomas): the first.** Link 1.2's `LOCAL_REQUEST` (`spec/LINK-PROTOCOL.md`
  §5.9) answers status, neighbours, the access list and the owner info with MeshCore's own bytes.
  Pile 3.9, task 02 (the Walter's pile 9 task 06 and meshmaster.dk's pile 4.27 task 176 use it).

  **Done on the radio's side, 7 November (p3.9-02).** `LOCAL_REQUEST` (`0x0D`), the capability bit
  `0x80`, `kProtoMinor` 2; the request answered by **upstream's own `MyMesh::handleRequest`** out
  of upstream's own `reply_data`, with not one file of MeshCore edited — both members are private
  at the pin and are reached by the explicit-instantiation idiom of
  `src/common/cxx/PrivateMember.h` (`spec/SYSTEM.md` S-2's *exception* of 7 November;
  `docs/upstream.md` place 9 is what a bump re-reads, and *Tier three* says a changed reply layout
  is a new link minor). Discovery and trace are **not** in 1.2: they are mesh exchanges with
  another node and have no local form, so those two Walter jobs for the unit's own repeater stay
  *Failed* and that is now the whole of what 1.2 left out. Telemetry stays `LPP_GET`'s.
  `docs/link.md` §2i is the page, `linkcli.py local …` reads it from a laptop, and the four lines
  a board has to show are `docs/bench.md` §4. The other two halves are still open: the Walter's
  pile 9 task 06 and meshmaster.dk's pile 4.27 task 176.

## Review of 01–02 (7 November)

An independent read of the code, the tests and the published files; nothing built. Checks run:
`linkref check` (88 cases on `link-v1.json`, 92 on `link-v1.1.json`), `specsync verify` (41 files),
`test_release.py` (84 OK), `test_walk` and `test_bench` (188 OK); the downloaded v0.1.1 files against
`SHA256SUMS` (9 OK), its signature against `keys/release.pub` (and a one-byte tamper refused), and every
UF2 block inside the application region with family 0xADA52840. Verdicts:

- **01:** correct.
  - `kProtoMinor` = 1. `HELLO` is answered at the lower of the two minors, the band is written only into
    a body at minor 1 or more, and `EV_BOOT` uses this firmware's own minor. The band comes from the
    descriptor, built from `TWINOAK_BAND`. The scanner speaks no link, so it has nothing to change.
  - `build_release.py`'s `link_version` now reads `kProtoMajor`/`kProtoMinor` from `Numbers.h`, and
    refuses if they are gone.
  - **A v0.1.0 Walter takes this image.** Its codec ignores trailing bytes, `descriptorAgrees` and
    `choose` compare the link's major only, and its health check's `HELLO` asks 1.0.
  - Byte for byte with the Walter's 04bede6.
- **02:** v0.1.1 is real and verifies. The tag is at 3d81536, the merge of #39. One gap in the recovery
  text matters (the first item below).

- [x] **[done by p3.9-01, 7 November: §3a's step 0 is `rak update status` and the box powered] *A radio left half-written* never mentions the Walter's
  journal.**
  - An unfinished *flashing-candidate* entry is resumed at the Walter's next boot (its `docs/update.md`,
    *After a power cut*). The section's own "power the box down and up" reboots the Walter. So a radio
    recovered by hand could be flashed over again with the journalled image.
  - Add a step 0: `rak update status` on the Walter first. It does no harm on the first unit, whose
    run ended *stuck*, not in progress.
  - Add that on the production adapter the RAK's USB-C does not power the radio: the box must be on
    (the hardware repository's `BENCH-BREADBOARD.md` §6 item 7).
- [x] **[done by p3.9-01: step 3 names the Walter's install and the `.uf2` of family 0xADA52840, and what each can write] Step 3 overstates what cannot be written.**
  - *Neither the Walter nor a `.uf2` can reach where the bootloader lives* is not true in general:
    `rak provision` writes the bootloader, and the bootloader's UF2 handler has a bootloader-family
    branch that stages a new one.
  - Narrow it to *the Walter's install, and this release's `.uf2` (family 0xADA52840)*.
  - The conclusion holds: neither writes 0xED000–0xF4000, so the key survives.
- [x] **[done by p3.9-01: `bootloader_app_is_valid` found in the 0.4.4 binary at 0xF5468, the same tests as 0.4.3's; the BLE OTA path noted as not read out] Step 1 was read from the RAK bootloader 0.4.3's source**; the radio has 0.4.4,
  whose one stated change is *BLE OTA recovery*, which may touch the invalid-application path. Check it
  in 0.4.4, or say what was checked.
- [ ] **[Thomas] The published v0.1.1 notes say *The Walter it goes with: None yet … pile 8,
  unreleased*.** The Walter's v0.1.1 was published at 06:14 UTC on 7 November. Edit the release body.
- Nits:
  - Report 02 lists sizes where the task asked for hashes; they are in the git-ignored `SHA256SUMS`,
    and were checked.
  - `verify_release.py`'s *NOT VALID against a key that is not it* proves nothing:
    `manifestref.verify` returns False on the key-id mismatch before any cryptography runs. The tamper
    test above covers the gap.
  - Report 01 says 396 native tests, report 02 says 394 for the same code (`out/verify/native.log`:
    394).
  - A 0.1.0 Walter copies `proto_minor` from `EV_BOOT`, so the radio's link minor it reports reads 1
    after a radio boot and 0 after its next `HELLO`. Harmless; nothing should key on it.
  - Spec (the reviewer's): `FIRMWARE-MANIFEST.md` §4 still speaks of *the same three strings* in
    `HELLO`; the band is a fourth.
  - Report 02's stale p3.7-01 bench sentence belongs here as an item.
- [x] **[done by p3.9-01: the cause is the core's `InternalFS.begin()` formatting a volume a torn page had made unmountable; the firmware now refuses and says so; §3a says what to expect and what to re-apply] The first radio did *not* keep its key.**
  - **What happened.** meshmaster.dk's audit log has `agent.companion_changed` for LTE test node from
    `ebf91b805ca34a50…9773174` to `776cdcd5ca758256…f0ef7ac2`. It arrived in the first heartbeat after
    Thomas recovered the half-written radio with v0.1.0's `.uf2` on 6 November (Walter 0.1.0+1f4e7f8,
    radio 0.1.0+2577a00).
  - **What is wrong in the docs.** `docs/flashing.md` *A radio left half-written* says the key
    survives, because neither the Walter's flasher nor the `.uf2` writes outside the application
    region. On the one radio it was tried on, it did not. So something reset the node's file system
    (`InternalFS`) or its identity between the start of the watchdog loop and the first boot after
    the `.uf2`.
  - **Find which.** Candidates:
    - the half-written application ran (read how the Walter's flasher orders the first page, and what
      bootloader 0.4.4 does with the settings page it left);
    - `InternalFS.begin()` formatting a volume it could not mount;
    - the identity file's path or format differing between the `0.0.0-dev` image that made the key
      and v0.1.0;
    - a page the flasher erased outside the image's region. The Walter's journal entry for that
      run's `check` carries a zero hash (Walter `tasks/phase-8/FOLLOWUPS.md`), so read what that
      run's image record was.
  - Until it is found, the guide says plainly that a recovery may give the radio a new identity, and
    how to tell: `rak hello` and `agent id` on the Walter, or the unit's companion key on
    meshmaster.dk.
  - **And the settings.** A radio that lost its file system booted as a fresh node: a random admin
    password nobody knows (S-13), the profile's lines and no name or position. Say what a person
    re-applies, and how: through the Walter's `rak cli`, or meshmaster.dk's jobs through the unit.

