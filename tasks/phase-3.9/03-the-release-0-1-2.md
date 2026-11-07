# 03 — The release 0.1.2

**Depends on:** 01 and 02 merged.
**First:** check that `keys/release.pub` holds the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-03.md` saying which, change nothing else, and end.
**Spec:** `docs/release.md`, `tasks/phase-3.8/02-the-release-0-1-1.md` and its report, the 3.8 review (`../phase-3.8/FOLLOWUPS.md`: hashes, not sizes, in the report; the notes' *The Walter it goes with*); the reports of 01 and 02; `spec/LINK-PROTOCOL.md`'s 1.2 paragraph.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Publish 01 and 02 as v0.1.2, so the radio in the field can be offered it over LTE: it has a golden
image now. 02 always changes the firmware (link 1.2), so there is always a release. Before 02 was
added this task released only if 01 changed something.

## Deliverables

1. **The release notes' hand-written top.**
   - What 01 found, and what it changed if anything, one sentence each.
   - Link 1.2: the repeater answers its own Walter's status, neighbours, access-list and owner-info
     requests with MeshCore's own bytes, so meshmaster.dk can read them through the unit.
   - Every sentence checked against the code.
   - *The Walter it goes with*:
     - the Walter's current release, whatever it is when this runs, from its repository's tags;
     - that a Walter before its v0.1.2 speaks link 1.1 and does not ask for these. Nothing breaks:
       the two meet at 1.1.
2. **The tag.** `v0.1.2` as `docs/release.md` says; CI green; published as **Latest**; the files
   verified as downloaded. The report has the URL and **every file's hash**.
3. **README's *The state of things*:** current.

## Acceptance criteria

- [ ] `v0.1.2` published as *Latest* and verified, with every hash in the report.
- [ ] The notes are checked against the code, including *The Walter it goes with*.
- [ ] README current.

## Out of scope

The board.
