# 01 — A bump can be green

**Depends on:** phase 3.1.
**Spec:** [`tasks/phase-0/FOLLOWUPS.md`](../phase-0/FOLLOWUPS.md) (the one open item, tagged `[reviewer]` — this task is the reviewer's answer: tier one stays reachable); [`docs/upstream.md`](../../docs/upstream.md) (*What a bump costs today*, the three tiers); `test/test_version/test_version.cpp` (`Version.ThisBuildSaysThePinOfPlatformioIni`, `Upstream.ThisBuildSaysThePinOfPlatformioIni`), `platformio.ini` (`[meshcore]`, and how `TWINOAK_MESHCORE_TAG` / `TWINOAK_MESHCORE_PIN` reach the native environment), `tools/release/test_release.py`, `tasks/sync/TEMPLATE.md` step 2, `.github/workflows/upstream.yml`.
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

The upstream watch opens a pull request when MeshCore releases, and CI on
that pull request is meant to be the whole test of a bump that changes
nothing we rely on: green, a person merges it, nobody opens a session. Two
tests make that impossible today — they assert the version and upstream
strings with the current tag and pin as literals, so every bump is red
before anything of upstream's has been read. Thomas wants tier one. Make the
two tests say what they mean — *the defines reach the strings* — without
naming a release.

## Deliverables

- **`test/test_version/test_version.cpp`**: the two `ThisBuildSaysThePinOfPlatformioIni`
  tests build their expected strings from the `TWINOAK_MESHCORE_TAG` and
  `TWINOAK_MESHCORE_PIN` macros (the first eight characters of the pin, as
  `formatUpstream` takes them), and additionally assert that the macros are
  **not empty** and that the pin is forty hex characters — so a build whose
  defines did not arrive still fails, which was the tests' point. One more
  test pins the *shape* with literals that no bump changes
  (`twinoak-repeater <version> on MeshCore <tag> (<8 hex>)`), fed with a
  made-up tag and pin through whatever seam `versionString()` has for its
  inputs; if it has none, say so and keep the macro-based assertion only.
- **`tools/release/test_release.py`**: the synthetic `buildinfo.json` reads
  the tag from `platformio.ini`'s `[meshcore]` section, or uses a value that
  is plainly synthetic (`repeater-vX.Y.Z`), so that it never restates a
  release either. The release tooling's own tests pass.
- **`docs/upstream.md`**: *What a bump costs today* rewritten — a bump that
  compiles and passes the native tests is green, and a person merges it; the
  list of what a bump *re-reads* (the seven places of `tasks/sync/TEMPLATE.md`)
  is what a reviewer looks at before merging a green one, and a red one is
  tier two. `tasks/sync/TEMPLATE.md` step 2 updated to match.
- **The follow-up ticked** in `tasks/phase-0/FOLLOWUPS.md` with one line
  naming this task — the one time a session, not the reviewer, ticks: the
  reviewer wrote this task as the decision.
- **A rehearsal**: run `.github/workflows/upstream.yml`'s rehearsal input
  (`pin_tag=repeater-v1.17.0`, as phase 0's task 03 did) and confirm CI on
  the rehearsal pull request is **green** with the two tests included; close
  and delete the rehearsal's pull request and branches afterwards; the report
  links the run.

## Acceptance criteria

- [ ] `tools/verify.ps1` green; `pio test -e native` passes with no literal tag or pin in `test_version.cpp` except in the shape test's made-up values (grep in the report).
- [ ] The rehearsal bump's CI is green; the report links it and says the pull request was closed and its branches deleted.
- [ ] `docs/upstream.md` and `tasks/sync/TEMPLATE.md` say a green bump is merged by a person without a session.
- [ ] The phase-0 follow-up is ticked with this task's name.

## Not verified here

The first real bump: the next `repeater-v*` tag MeshCore publishes.

## Out of scope

Anything else the upstream watch does. The phase 3 review (the reviewer's,
separately).
