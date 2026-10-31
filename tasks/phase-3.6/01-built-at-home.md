# 01 — Built at home

**Depends on:** phase 3.5.
**Spec:** this repository's `.github/workflows/ci.yml` (one job: the boards, the native tests, the release tooling's own tests), `upstream.yml` (the weekly MeshCore check), `release.yml` (`ci-green`, `build`, `publish`; it holds `RELEASE_SIGNING_KEY`); `keys/README.md`; the pattern, meshmaster.dk's move of 29 October (read-only, its `master` on GitHub: commits `7d84fc9`, `b7c9bd9`, `35a5d2a`, `65ce46a`, `70b73ad`, and its `ci.yml` as it stands there); `tasks/RUNBOOK.md`; wherever `README.md` or `docs/` describe CI and the release.
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

Thomas, 31 October: *all CI is to run on our new Unraid runners.* These are
self-hosted at the organisation level, label **`unraid`**. meshmaster.dk moved
its CI to them on 29 October, and every job here still runs on `ubuntu-latest`.

What meshmaster.dk learned on the way:

- `runs-on: [self-hosted, unraid]`.
- **A self-hosted runner keeps its home between jobs**, so its cache steps run
  only `if: runner.environment == 'github-hosted'`.
- **The runner shares the host**, so anything a job starts there is cleared
  at the start of the job and taken down `if: always()`.
- **pwsh, gh, jq, node, curl and docker are on the runners**, because its CI
  uses them there.

**What ships is built away from home.** The runner is shared with every branch
a task session pushes, and keeps its home, including `~/.platformio` and its
toolchains, from one job to the next. `release.yml` builds the images every
repeater's update trusts, and signs them with `RELEASE_SIGNING_KEY`
(`keys/release.pub`). Signing elsewhere would keep the key off the runner, but
it would still sign whatever the runner built. So the whole of `release.yml`
stays on GitHub's runners. It runs at a tag, so its minutes are few.
Everything that **checks** goes home; everything that **ships** stays.

## Deliverables

1. **Home:** `ci.yml`'s job and `upstream.yml`'s job run on
   `[self-hosted, unraid]`.
2. **Away:** every job of `release.yml` stays on `ubuntu-latest`, with a
   two-line comment at the top of the file saying why.
3. **Caches as meshmaster.dk has them:** in `ci.yml`, the PlatformIO and pip
   cache steps run only on GitHub's runners, with a comment saying why.
   `setup-python` and the pinned installs stay. Check what the job calls that
   the runners may not have (`zip`, the release tooling's requirements);
   install what is missing in the job, rather than assume it is there.
4. **Nothing left behind, nothing shared:** check that no job writes outside
   its workspace and the runner's home or uses a host port, and that
   checkout's clean leaves no `.pio/` behind.
5. **The guard.** A step early in `ci.yml` (pwsh or python, as the workflows
   already use) checks every job in `.github/workflows/`. It fails, naming the
   file and the job, when a job outside `release.yml` runs anywhere other than
   `[self-hosted, unraid]`, or when a job of `release.yml` runs anywhere other
   than GitHub's runners. The rule and its reason sit in one place, so neither
   change happens by accident.
6. **Words:** where the README or `docs/` describe CI and the release, and
   `keys/README.md`: where each workflow runs and why, and what a runner must
   have.

## Acceptance criteria

- [ ] This task's pull request runs `ci.yml` on a runner labelled `unraid`; the report quotes the runner names from the *Set up job* logs; `tools/verify.ps1` green.
- [ ] The guard fails on a scratch commit that puts `ci.yml`'s job on `ubuntu-latest`, and on one that puts a `release.yml` job on `[self-hosted, unraid]` (both reverted; the report quotes the failures).
- [ ] After merge, `upstream.yml` runs once by `workflow_dispatch` at home (this task if the token allows, else the report asks Thomas to).

## Out of scope

The runners themselves. The signing scheme and what the release does. The
bench and its board, which have nothing to do with CI.
