# 03 — The release 0.1.3

**Depends on:** 01 and 02 merged.
**First:** check that `keys/release.pub` holds the key `73efdd383a957596` and that `gh secret list` shows `RELEASE_SIGNING_KEY` (names only, never a value). If either has gone, write `logs/STOPPED-03.md` saying which, change nothing else, and end.
**Spec:** `docs/release.md`, `tasks/phase-3.9/03-the-release-0-1-2.md` and its report (hashes, not sizes; the notes' *The Walter it goes with*); the reports of 01 and 02.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Publish 01 and 02 as v0.1.3, so the radio in the unit can be offered it over LTE.

## Deliverables

1. **The release notes' hand-written top.**
   - What the totals are and why: a reading is a moment, and a unit is only ever asked while its
     Walter is awake. One short paragraph, with 02's table.
   - What they are not: a fuel gauge.
   - That channels 1 to 5 are unchanged.
   - Every sentence checked against the code.
   - *The Walter it goes with*: the Walter's current release, whatever it is when this runs, from
     its repository's tags. It needs no change; it passes the telemetry on as it is.
   - *meshmaster.dk*: until its own pile, the nine show there as plain channels.
2. **The tag.** `v0.1.3` as `docs/release.md` says; CI green; published as **Latest**; the files
   verified as downloaded. The report has the URL and **every file's hash**.
3. **README's *The state of things*:** current.

## Acceptance criteria

- [ ] `v0.1.3` published as *Latest* and verified, with every hash in the report.
- [ ] The notes are checked against the code, including *The Walter it goes with*.
- [ ] README current.

## Out of scope

The board. Updating the unit's radio: a person starts that on meshmaster.dk.
