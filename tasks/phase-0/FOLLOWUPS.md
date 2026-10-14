# Phase 0 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

- [x] **[reviewer]** `test/test_version/test_version.cpp`'s
  `Version.ThisBuildSaysThePinOfPlatformioIni` asserts the version string with
  the pin and the tag as literals (`repeater-v1.17.1 (d9296435)`), so **every**
  MeshCore bump is red in CI before anything of upstream's has been looked at —
  which turns tier one of `docs/upstream.md` (a green bump a person merges) into
  tier two (a session) for free. Task 03 did not touch it: it is task 01's file,
  and task 03's own acceptance criterion says nothing `tools/verify.ps1` runs
  changed. The one-line alternative is to assert against the
  `TWINOAK_MESHCORE_TAG` / `TWINOAK_MESHCORE_PIN` macros instead of the
  literals, which keeps the test's point — *the defines reach the string* —
  without pinning it to one release. `tools/release/test_release.py` names the
  old tag in a synthetic `buildinfo.json` too, which is cosmetic.
  `docs/upstream.md` (*What a bump costs today*) and `tasks/sync/TEMPLATE.md`
  step 2 both say so, so a bump is cheap either way; whether tier one should be
  reachable at all is the reviewer's call.
  **Done by `tasks/phase-3.2/01-a-bump-can-be-green.md`**, which the reviewer
  wrote as that call: the two tests build their strings from the
  `TWINOAK_MESHCORE_TAG` / `TWINOAK_MESHCORE_PIN` macros and assert the defines
  arrived, `tools/release/test_release.py` names no release, and
  `docs/upstream.md` and `tasks/sync/TEMPLATE.md` say a green bump is merged by
  a person with no session (`tasks/phase-3.2/reports/01.md`).
