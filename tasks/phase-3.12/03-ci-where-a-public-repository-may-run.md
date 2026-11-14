# 03 — CI where a public repository may run

**Depends on:** nothing. **It runs first:** 01's pull request (#51) and 02 both wait for it.
**Spec:** `.github/workflows/ci.yml` (its one job, `verify`, is `runs-on: [self-hosted, unraid]`; the steps that make a shared runner fit to use), `upstream.yml` (~L59, the same), `release.yml` (its three jobs are on `ubuntu-latest` already), `tools/ci/check-runners.ps1` (the rule ~L8–16: *what CHECKS runs at home, what SHIPS runs away*; `$atHome` ~L44; the failures ~L144–180) and whatever tests it, `docs/release.md` (~L17), README, `tasks/phase-3.12/logs/STOPPED-01.md`; **the exit-node firmware, read only, as the model:** `tools/check-runners.py` (its header: one rule, every job on a GitHub-hosted runner, and why) and `tasks/phase-1.8/29-built-at-home.md` with its report.
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

This repository became public on 13 November, between 16:05 and 16:52 UTC. Its CI runs on the
organisation's Unraid runners, and the organisation's one runner group is *All repositories,
excluding public repositories*. Since that hour no job of `ci` or `upstream` has been given a
runner. Runs sit queued, the required `verify` check never reports, and no pull request can be
merged. Task 01 of this pile is finished and waits as #51.

The group's setting is right and stays: a self-hosted runner on a public repository runs a
stranger's pull request on the host. The exit-node firmware has been public longer and has the
rule in `tools/check-runners.py`: every job on a GitHub-hosted runner. This repository takes the
same rule. Standard hosted runners cost a public repository nothing.

## Deliverables

1. **Every job of every workflow runs on a GitHub-hosted runner.** `ci.yml` and `upstream.yml`
   move; `release.yml` is there already.
2. **`tools/ci/check-runners.ps1` holds the new rule** and refuses `self-hosted` anywhere, with
   its reasons in its header as the exit node's has them. Its tests follow.
3. **What `ci.yml` does because a runner at home is shared and long-lived** (the check for
   leftovers, installing what may be missing) is kept where it still earns its place and removed
   where it does not. Say which, and why.
4. **The job is still called `verify`**, so `main`'s required check is the one it reports.
5. **This task's own pull request is the proof.** Its `ci` run starts on a hosted runner and goes
   green. Put the run and its duration in the report, beside the last run at home (2 m 59 s).
6. **The words.** README, where it says where CI runs; `docs/release.md`. No line in
   `release-notes/unreleased/`: nothing a user of the firmware sees changes.

## Acceptance criteria

- [ ] No workflow names `self-hosted`, and the rail fails a planted one (test).
- [ ] `verify` is green on this task's pull request, from a GitHub-hosted runner; the run is in the report.
- [ ] After the merge the `ci` run of `main`'s head is green, so the release's *ci is green* gate has something to stand on again; that run is in the report too.
- [ ] `tools\verify.ps1` green.

## Out of scope

The organisation's runner group, which is Thomas's and is right as it is. meshmaster.dk, which
is private and stays at home. Pull request #51: it is merged by a person once this has landed
(README, *Order*).
