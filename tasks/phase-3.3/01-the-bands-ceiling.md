# 01 — The band's ceiling

**Depends on:** phase 3.2.
**Spec:** the phase-3 review ([`../phase-3/FOLLOWUPS.md`](../phase-3/FOLLOWUPS.md), *Review of 01–03*: the duty cycle, `linkloop`) and the 3.2 review ([`../phase-3.2/FOLLOWUPS.md`](../phase-3.2/FOLLOWUPS.md): *seven places*, the paging arithmetic); ERC Recommendation 70-03, Annex 1, the 869.40–869.65 MHz band (500 mW e.r.p., duty cycle ≤ 10 %) — cite the edition you read; `platformio.ini` (`[band_868]`), `src/common/bands/Bands.h`, `BandCheck.cpp`; `src/roles/repeater/firstboot/Profile.{h,cpp}` (the table, `why`), `test/test_firstboot/`, `test/test_rails/` (rule 4's profile half); `docs/ARCHITECTURE.md` (the duty-cycle section, ~L468–490: the mesh client's ceiling against upstream's budget), `docs/provisioning.md` §2 (*What this page deliberately does not set*), `docs/bench.md` §10; MeshCore at the pin: `CommonCLI::handleSetCmd`'s `dutycycle` and `af` branches and the matching `get` (`src/helpers/CommonCLI.cpp:450-461`, `:809-814` per the architecture page), `MyMesh.cpp:890`; `tools/verify.ps1` (the native step and its container), `tools/linkloop/check.sh`; `.github/workflows/upstream.yml` (~L162), `test/test_settings/` (`SensorList.*`), `src/roles/repeater/app/Settings.cpp`.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The home channel's band allows a 10 % duty cycle and upstream starts a
repeater at 50 %. The limit is a property of the band, like its frequency
and its power, and a fresh unit should start inside it. Put it in the band,
let the first-boot profile ask for it, and close three small gaps the reviews
found.

## Deliverables

1. **The duty cycle is the band's.** `[band_868]` gains `duty_cycle = 10`
   with the regulation in its comment, `Bands.h`'s row carries it, and
   `BandCheck.cpp` refuses a band without one or outside 1–100. The profile's
   second line is **built from the band**, not written: `set dutycycle 10`
   with its `why` naming the band and the regulation. A test holds the line
   to the band's value; rule 4's rails test learns that the profile has two
   lines. Check upstream's `dutycycle` branch at the pin for what it writes
   and replies (`airtime_factor = 100 / dc - 1`, and the reply the reply rule
   must accept), and that a `get` of it reads back what was set.
2. **What 10 % does to our own traffic.** `docs/ARCHITECTURE.md`'s
   duty-cycle section works the numbers again at the band's ceiling: upstream's
   budget is then 360 s an hour, and the mesh client's ceiling of six maximum
   packets a minute (about 13 s a minute) is above it. Say what then happens
   (upstream delays; nothing is refused) and decide whether the client's
   ceiling should follow the band's budget — if it should, lower it in the one
   constant, with a test, and say what a server's exchange (about 2 %) still
   fits. `docs/link.md`'s row moves with it.
3. **The pages say what a unit starts at.** `docs/provisioning.md` §2 no
   longer files the duty cycle under the neighbourhood's judgements: the
   band's ceiling is set, and a *lower* value is still a site's choice; a unit
   provisioned before this keeps 50 % until `set dutycycle 10` is sent (say so,
   and add it to the USB session of §3). `docs/bench.md` §10's first line
   reads `get dutycycle` (or whatever upstream's `get` spells) → `10`.
4. **`tools/linkloop/check.sh` runs in the native step**, in the same
   container and with the same compiler, so a stale include fails
   `tools/verify.ps1`. Its miniature of upstream's `set` learns `dutycycle` if
   `provision`'s cases need it.
5. **`upstream.yml`'s pull-request body says eight places**, and names the
   eighth (what our sources take from upstream's headers) as
   `docs/upstream.md`'s table does.
6. **The paging arithmetic does not depend on today's tag.** `test_settings`'
   `SensorList.*` build the version string from the real format at the
   longest tag and pin the format allows (or assert the bound the arithmetic
   relies on), so a longer upstream tag fails a test before it changes what a
   device prints.

## Acceptance criteria

- [ ] `[band_868]` carries the duty cycle; a band without one does not compile; the profile's two lines are what `test_firstboot` and `test_rails` expect, and the second is generated from the band.
- [ ] The architecture page's numbers are redone at 10 % and the client's ceiling is either unchanged with the reason or lowered with a test.
- [ ] `tools/verify.ps1` runs `tools/linkloop/check.sh` and fails when an include in it is broken (show it once, then restore).
- [ ] `tools/verify.ps1` green; both images' sizes in the report (the scanner's unchanged).

## Not verified here

A board answering `get dutycycle` after a first boot — task 03 of this phase
walks §10 when the bench is there and the identity may go.

## Out of scope

The rest of the Danish standard (`flood.advert.interval` from the key,
`agc.reset.interval`, `loop.detect`, `flood.max.unscoped`): Thomas's to
choose, in the phase-3 review. The guest password (S-13).
