# 01 — A release from the button

**Depends on:** nothing.
**First:** read only. `gh api repos/twinoak/twinoak.meshcore.repeater-firmware/actions/permissions/workflow` must say the workflow's own token may create pull requests. If it may not, write `logs/STOPPED-01.md` naming the setting (*Settings → Actions → General → Allow GitHub Actions to create and approve pull requests*), change nothing else, and end: a setting is Thomas's to change.
**Spec:** `.github/workflows/release.yml` (the two triggers ~L55; the version from the tag ~L104; the three refusals of a tag ~L118–150: not on `main`, no `RELEASE_SIGNING_KEY`, no hand-written notes; the body in two parts ~L314; `gh release create` ~L433), `.github/workflows/ci.yml`, `tools/release/` (`release_notes.py` with `--require --check`, `build_release.py`, `sign_manifest.py`, `selfcheck.py`, `test_release.py`), `docs/release.md` (*Making one*, *The notes*, *The key*, *When a tag is refused*, *Versions*), `release-notes/` (four hand-written tops, each with *The Walter it goes with*), `tasks/RUNBOOK.md`, `tools/verify.ps1` (the local dry run), `tasks/phase-3.10/03-the-release-0-1-3.md` and its report (everything a release needed by hand); **meshmaster.dk, read only, as the model**: its `.github/workflows/release.yml` (*Compile the release notes* ~L168; the pull request that files them, opened and merged by the job's own token, ~L341–410), `tools/ci/Compile-ReleaseNotes.ps1`, `release-notes/unreleased/`, and its `tasks/RUNBOOK.md` on a task's release note.
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

See the README. A release of this firmware has needed a task: the notes' top written by hand as
`release-notes/vX.Y.Z.md`, a tag pushed, and somebody to know that *Run workflow* is not it. After
this task it needs one press.

## Deliverables

1. **The version is in the repository, in one place, and it names the next release.** Today that
   is `0.1.4`. The workflow reads it from there and from nowhere else, and the image's descriptor
   and the manifest carry it with the commit as now.
2. **A task leaves a line.** `release-notes/unreleased/<task>.md`: what a person with a radio will
   notice, in a sentence or two. `tasks/RUNBOOK.md` gets the rule in meshmaster.dk's words: a
   change somebody will notice has a line, a change nobody will has none.
3. **`workflow_dispatch` publishes**, from `main`'s head and no other commit:
   - CI green for that commit, and signed with `RELEASE_SIGNING_KEY`, both as now. A release
     without the key still fails;
   - the body's top is the unreleased lines, in the order the tasks merged, where the
     hand-written file was. The generated part below it is unchanged;
   - the release is created with its tag on that commit, as **Latest**;
   - a tick box **dry run** does what a run by hand does today: the same jobs, the test key,
     `0.0.0-dryrun`, nothing published. Only a dry run may name another `ref`.
4. **What every top said by hand.** *The Walter it goes with*, which MeshCore this is, the link to
   the flashing guide: whatever can be generated is generated, and what cannot is a standing
   paragraph in the repository that the task which changes it edits. This repository's token
   cannot read the Walter's repository; do not make a release depend on that.
5. **It refuses, before anything is built, and says why in one sentence:**
   - no unreleased line: there is nothing to release;
   - the tag of the version already exists: the version was not moved on after the last release,
     and the sentence says how to finish that;
   - a release asked for from a ref that is not `main`.
6. **After the release is out, one pull request, opened and merged by the workflow:** the lines
   move from `unreleased/` into `release-notes/vX.Y.Z.md`, as published, and the version moves to
   the next patch. Where `main`'s rules do not let the workflow merge it, the pull request is left
   open and the run's summary says so. The release stands either way, and the next one refuses
   until the version has moved.
7. **The commit that pull request makes** touches only the release's own files. Say how the
   *ci is green* gate reads it at the release after: it has no CI run of its own.
8. **The tag trigger goes.** One way to release. A tag pushed by hand starts nothing.
9. **The parts that can be tested are.** Compiling the lines, the refusals and the move after a
   release, in `tools/release/` beside the tests that are there, against a scratch repository.
   `tools/verify.ps1`'s local dry run follows the new path. A dry run from this task's branch is
   in the report.
10. **The words.** `docs/release.md`, rewritten where it says *tag*; `tasks/RUNBOOK.md`; README;
    and this task's own line in `release-notes/unreleased/`.

This task publishes nothing and pushes no tag. The key's private half stays where it is: in the
secret, and in no file, no log and no commit.

## Acceptance criteria

- [ ] The version is read from one place, and a build's descriptor and manifest carry it (test).
- [ ] Two unreleased lines compile into the body's top in merge order; none is a refusal with its sentence (tests).
- [ ] An existing tag of the version, a ref that is not `main`, and a release with no key are each refused before the build (tests).
- [ ] After a release in a scratch repository: the lines are in `release-notes/vX.Y.Z.md`, `unreleased/` is empty and the version is the next patch (test).
- [ ] The four published notes in `release-notes/` are untouched.
- [ ] The dry run from this branch is green and published nothing; its run is in the report.
- [ ] `release.yml` has no tag trigger and still runs on GitHub's own runners.
- [ ] `tools/verify.ps1` green. CI green.

## Out of scope

A release. The board. The Walter's and the exit node's repositories, which get the same in their
own piles.
