# 05 — The Danish standard

**Depends on:** 04.
**Spec:** `spec/SYSTEM.md` S-12, S-13; `docs/provisioning.md` (§2 and the profile's table); `src/roles/repeater/firstboot/Profile.h/.cpp` (the table, the rule of what may be a line, the *deliberately not here* list — `flood.max.unscoped` is on it today — the reply rule, `runProfile`), `src/roles/repeater/app/FirstBoot.cpp` (when the profile runs, and whether the node's key exists by then), `test/test_firstboot`; `docs/bench.md` §10 and walk 10 in `tools/bench/walk.py`; `tasks/phase-3/reports/03.md` (every `set` key of the pinned CLI with upstream's default and the Danish standard's value: `agc.reset.interval <s>` 0 → 4, `flood.advert.interval <3-168 h>` 47 → 60–85 from the key, `flood.max.unscoped <=64` 64 → 15, `loop.detect off|minimal|moderate|strict` off → moderate); [`../phase-3/FOLLOWUPS.md`](../phase-3/FOLLOWUPS.md), *[Thomas] The rest of the Danish standard* (decided — this task); MeshCore at the pin (`src/helpers/CommonCLI.cpp`'s `set` handlers for the four keys) **and at `repeater-v1.18.0`**, which task 06 bumps to tonight: `gh api "repos/meshcore-dev/MeshCore/contents/src/helpers/CommonCLI.cpp?ref=repeater-v1.18.0" -H "Accept: application/vnd.github.raw"` (and the other files the handlers lead to). 1.18 touched two of these keys by title: *Constrain `agc.reset.interval` and allow intervals > 255s* (PR #3469) and *treat transport code 0 the same as un-scoped* (`01f9293b`).
**Size:** one session.
**Model:** claude-opus-5

## Decision (Thomas, 22 October)

A fresh unit gets all four of the Danish standard's settings that phase 3's review left to him:

- **`flood.advert.interval`**: 60–85 h, chosen from the node's own key (upstream's default is 47), so
  that units flashed together do not advertise in step across the mesh;
- **`agc.reset.interval 4`**;
- **`loop.detect moderate`**;
- **`flood.max.unscoped 15`**: now a default, which a site may still change at provisioning. It comes
  off the *deliberately not here* list; the three delays stay on it.

## Goal

The profile is short on purpose: a line is in it only where a document of this project gives the
value. These four now have one: Thomas's decision, from the Danish standard that phase 3's report
tabled. Put them in the profile, with the advert interval derived from the node's key, and walk a
first boot on the board.

## Deliverables

1. **Three static lines** in `kProfile`: `set agc.reset.interval 4`, `set loop.detect moderate` and
   `set flood.max.unscoped 15`. Each one's `why` names the Danish standard and this decision. The
   *deliberately not here* comment is updated.
2. **One line built from the key:** `set flood.advert.interval <h>`.
   - **The rule:** `h = 60 + (k mod 26)`, with `k` taken from the public key by a rule this task defines
     and `docs/provisioning.md` states (for example, its first byte). The same key always gives the same
     hours, and all of 60–85 can be reached. Check at the pin that the key takes hours and that 85 is
     inside its range.
   - **The runner:** the table stays text wherever it can. Make the smallest change to `runProfile`'s
     interface that lets one line be built at run time and keeps the table readable and tested.
   - **Timing:** the line is built only after the key exists. Read `FirstBoot.cpp`. If the key is made
     after the profile runs, either the order moves or this line runs once the key exists; say which,
     and why.
   - **Native tests:** the bounds, the mapping, determinism, and the line's text.
3. **Both pins.** Each line must be accepted under the reply rule at 1.17.1 (the build), and by reading
   at 1.18.0. If a line's meaning or range differs between the two, say so in the report and leave an
   **`[06]`** item in this phase's `FOLLOWUPS.md` for the bump to settle. A key the pin does not have is
   not added here; it becomes an `[06]` item instead.
4. **`docs/provisioning.md`:** the profile's table with the four lines, the advert-interval rule, and
   what a site may change afterwards (`flood.max.unscoped` and the rest).
5. **On the board:** `bench walk 10` (the identity is disposable). The first boot must:
   - accept every line of the profile (`firstboot.state` reports none refused);
   - give a `get flood.advert.interval` equal to the value the walk computes from the `Repeater ID` it
     read;
   - read back `agc.reset.interval`, `loop.detect` and `flood.max.unscoped` as set.

   Walk 10's steps grow those four `get`s. Then `--tick`.
6. **What stays out:** no password and no per-site value (name, position, the delays) enters the
   profile. The existing tests hold this; extend them if the generated line needs it.

## Acceptance criteria

- [ ] The four lines in the profile, the advert interval derived from the key, with native tests (bounds, mapping, determinism).
- [ ] Each line checked at both pins, and any difference left as an `[06]` follow-up.
- [ ] `docs/provisioning.md` updated.
- [ ] Walk 10 on the board: every profile line accepted, and the four values read back as set, the interval as computed.
- [ ] `tools/verify.ps1` green.

## Out of scope

The guest password (still `[Thomas]` in phase 3's follow-ups). The delays, which depend on the site.
The bump itself (06).
