# 02 — The release says what is left

**Depends on:** 01 (nothing shared; one pull request at a time).
**Spec:** `.github/workflows/release.yml` (the last step ~L500–590: the pull request's body ~L530–545 with *No check runs on this pull request* ~L537; the three ways it ends `exit 1`: the branch not pushed ~L551, the pull request not opened ~L562, not merged after ~L574), `tools/release/` (`release_notes.py`, `gate.py`, their tests), `docs/release.md`, `tasks/phase-3.11/reports/01.md` (~L148–165: `main` requires `verify`); **the Walter firmware, read only, as the model:** its `.github/workflows/release.yml` ~L428–462 (`waiting`: the run stays green and its summary says what is waiting) and its `docs/release.md` (*Expect to merge that pull request by hand*).
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

On 13 November Thomas released v0.1.4 with the button. The release was right. The run was red,
the pull request that files the notes sat at *verify — Expected*, and an hour later a second red
run appeared. He asked whether he had broken the repository.

What happened, as seen on github.com:

- `main` requires the `verify` check, so the job cannot merge its own pull request. That is
  every release, and 3.11's report says so.
- GitHub **does** start a `ci` run for that pull request, and holds it for a maintainer's
  approval (run 43467131534). Unapproved, it expires after about an hour and is listed as
  failed. The pull request's body says no run starts.
- The step ends `exit 1`, so a release that published is a red run.

## Deliverables

1. **A release that published is a green run.** Where the notes' pull request is open and the
   job cannot merge it, the step ends as the Walter's does: green, with one line at the top of
   the run's summary saying what is left for a person, and which pull request.
   - Still red: the branch could not be pushed, or the pull request could not be opened. Nothing
     is waiting for a person then; something failed.
2. **How the step ends is decided in one small place with a test:** pushed or not, opened or
   not, merged or not, in; the exit and the summary's first line, out.
3. **The pull request says what is true:** that `main` requires `verify`; that GitHub holds the
   run for a maintainer's approval; and the two ways to finish, either of which is right: approve
   the run and merge when it is green, or merge without waiting, because the commit touches
   `VERSION` and `release-notes/` and nothing else. A few lines.
4. **The next press still refuses** while that pull request is open, in the sentence it has.
5. **`docs/release.md`** says the same where a person looks after a release.

No line in `release-notes/unreleased/`: nothing a user of the firmware sees changes.

## Acceptance criteria

- [ ] Pushed, opened, not merged: green, and the summary's first line names the pull request (test).
- [ ] Not pushed, and pushed but not opened: red, each with its own sentence (tests).
- [ ] The pull request's body no longer says that no check runs, and a rail holds it (test).
- [ ] The dry run from this branch is green and published nothing; its run is in the report.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

`main`'s rules: a repository setting is Thomas's to change. A release.
