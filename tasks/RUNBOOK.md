# Runbook — one task, unattended

You are Claude Code running headless (`claude -p`): nobody is watching, nobody
answers questions. `tasks/run-all.ps1` started you with "Run task NN of phase
P". Everything below uses `tasks/phase-P/` for that phase. Do exactly this,
in order. Everything you decide goes into files in the repo, never into the
console.

This is twinoak/meshmaster-agent-firmware's runbook, for this repository. Two
things differ from it and are worth knowing before the rest: the trunk here is
**`main`**, and **verification is local first** (§2) — CI confirms what the
machine you are on has already shown.

## 0. Orient

- You are on `main`, clean and up to date (the driver checked). If not:
  `git checkout main && git pull --ff-only`.
- Read `tasks/README.md`, `tasks/phase-P/README.md`, then
  `tasks/phase-P/NN-<name>.md`, then every section of `spec/` and `docs/` that
  task references, in full. **`spec/` wins over the task file**; note any
  disagreement in the report. `spec/` is a copy and is never edited here
  (`spec/README.md`).
- Read `tasks/phase-P/FOLLOWUPS.md`. Every open item tagged `[NN]` for your
  task is part of your task. Items tagged for other tasks are not yours; do
  not tick anything — the reviewer does.
- Read `tasks/phase-*/reports/*.md` for the tasks yours depends on: their
  "deviations" are what you inherit. Phase 0's task 01 report is the one to
  read before touching `platformio.ini`, CI or the release.
- Read `docs/upstream.md` (once phase 0 has written it) before changing
  anything that cites a line of MeshCore. The pin is in `platformio.ini`'s
  `[meshcore]`, and the source it names is on disk under
  `.pio/libdeps/<env>/MeshCore` after a build — **read the firmware you build
  against** rather than trusting a transcription of it, this file's and the
  task's included.

**Other repositories.** A task may tell you to read a file of a sibling
repository at a commit. Read it without touching that repository's working
tree — other sessions are working in some of them:

```bash
gh api "repos/twinoak/<repo>/contents/<path>?ref=<commit>" -H "Accept: application/vnd.github.raw"
# or, from the sibling clone, a read that changes nothing:
git -C ../<repo-folder> show <commit>:<path>
```

Never check out, pull, commit or write in a sibling repository. The two
MeshMaster repositories (`meshmaster.dk`, `twinoak.meshmaster.exitnode-firmware`,
named `meshmaster-agent-firmware` until 11 October 2026) and the
hardware repository are read-only to every task here.

## 1. Branch

`git checkout -b task/pP-NN-<name>` from `main`. One task, one branch.

**If that branch already exists** (locally, or on `origin` — check both), a
previous session of this task was cut off: a usage limit, a crash, a closed
lid. Do not delete or reset it and do not start over. `git checkout` it,
`git pull` if it has a remote, read `git log main..HEAD` and any draft of
`reports/NN.md` — the reviewer may have moved one to
`tasks/phase-P/logs/NN-report-draft.md`, which is git-ignored so that an
unfinished report cannot reach `main` and make the driver think the task
landed — run the verification to see where it stands, and continue from
there. The commits already made are the task's; yours are added after them.
Say in the report that the task was done in more than one session.

## 2. Implement and verify — locally, first and for real

- The conventions in `tasks/README.md` apply: spec wins, no scope creep,
  libraries pinned to an exact version with a one-line justification, secrets
  never in the repo, docs move with code, leave the evidence.
- **One command verifies the repository: `tools/verify.ps1`** (phase 0's task
  01 writes it; until it exists, the task's own commands are the
  verification). It runs what CI runs — every board environment's build, the
  native tests, the check of `spec/` against its manifest — and CI runs *it*.
  Run the task's own verification commands as well. Fix until green.
  `-Werror` is on for our own sources (`build_src_flags`): a warning in `src/`
  is a red build.
- **Green locally is the bar for pushing.** Do not push to see what CI says.
  GitHub Actions minutes are not where this project debugs.
- **No native compiler on Windows.** `pio test -e native` is a host build and
  this machine has no `g++`; `tools/verify.ps1` runs it in a container, and by
  hand it is:

  ```bash
  docker run --rm -v "C:/Users/<you>/path/to/twinoak.meshcore.repeater-firmware:/w" -w /w python:3.11-slim \
    bash -c "apt-get update -qq && apt-get install -y -qq g++ git && pip install -q platformio && pio test -e native"
  ```

  In Git Bash, prefix that with `MSYS_NO_PATHCONV=1` or the `-w /w` is
  mangled into a drive letter.
- **Run the long commands in the foreground and let them finish inside the
  same tool call.** A clean `pio run` fetches a toolchain and every library
  and takes minutes; raise that call's timeout to suit. Never start one in the
  background and then end your turn to wait for a completion notification: a
  headless session with nothing left to do *ends*, and the task dies there —
  the run unfinished, the report unwritten, the branch unpushed. Block on it
  inside the turn, or poll it inside the turn.
- **A session cannot hear a radio, and it touches a board only through
  `tools/bench/`** — only when its task says so, never with an RF port on an
  antenna for a repeater image, and it leaves the board as it found it
  (`bench restore`, whose last line says what went back). The rig is
  `bench.local.json` at the repository root, by USB serial number
  (`bench.example.json`; `docs/bench.md` §0a). *No bench* — no file, or no
  port with that serial number — is exit code 3 and **is not a failure**: the
  walk goes to *Not verified here*, as before this tool existed. The tool
  flashes one kind of file (the DFU package the build or the release made)
  after its own checks, never a UF2, never SWD, never an erase, never
  `reboot 2`/`3`; its console refuses to send a password and never prints
  one. Nothing else on a session's machine opens a board's port. A session can
  otherwise build, run the native tests, and read the MeshCore source it
  builds against; everything else is Thomas's to walk with the rig on the
  bench, so every task ends with a *Not verified here* list precise enough to
  be walked, and the same lines are added to `docs/bench.md` in the order the
  rig is brought up (`spec/SYSTEM.md` §7). Four of that page's sections are
  already walks — `bench walk 1`, `8`, `9`, `10` (`docs/bench.md` §0b) — so a
  task whose *Not verified here* is one of their lines runs the walk when the
  board is there, puts the result file in the report and ticks the line with
  `--tick`; everything the walk calls *needs a person* stays in the list.
- Nobody will answer a question. Where the spec and the task are silent, take
  the smallest decision consistent with the spec and record it in the report
  under "Decisions taken without asking".
- Never: `git push --force`, delete a branch other than your own, write a
  secret anywhere (including the report and the logs), edit a file under
  `spec/`, edit a file of the pinned MeshCore checkout, or enable
  `MESH_PACKET_LOGGING` / `MESH_DEBUG` — nothing the mesh says is logged as
  text, and a test greps for them.

## 3. Report

Write `tasks/phase-P/reports/NN.md` and commit it on the branch. It is the
PR description and what the reviewer reads:

```
# pP-NN — <name>

## Acceptance checklist
- [x] <criterion> — <one line: how it was verified>
...
## Deviations from the spec or the task
## Decisions taken without asking
## New libraries (with the one-line justification)
## Not verified here (and why)      <- the bench list: what Thomas must do with the rig
```

## 4. PR, CI, merge

**With `gh` (installed and logged in):**

1. `git push -u origin task/pP-NN-<name>`
2. `gh pr create --title "pP-NN: <name>" --body-file tasks/phase-P/reports/NN.md`
3. `gh pr checks --watch --fail-fast`, **in the foreground, in calls of at most
   nine minutes**: a CI run takes ten to fifteen minutes here, and one tool call
   may not take more than ten, so `timeout 540 gh pr checks --watch --fail-fast`
   and, when it times out, the same again - until it answers. Never in the
   background, never with a monitor, never by ending the turn to be told: a
   headless session is woken by nothing, and p3-06's first session ended that
   way with its pull request open and unmerged (the driver now continues such a
   session once or twice, but a nudge is a second session's cost).
4. Green: `gh pr merge --merge --delete-branch`, then
   `git checkout main && git pull --ff-only`.

**CI is red after a green local run.** That is a finding about
`tools/verify.ps1`, not a place to iterate. `gh run view --log-failed` on the
failing run; find what CI ran that the local script did not, or ran
differently; **fix the script so that it would have caught it**, reproduce the
failure locally, fix it locally, and push once. The report says what the gap
was, under "Deviations". **One round.** Red a second time is a stop condition
(§5). The only exception is phase 0's task 01, which writes CI itself and has
three rounds for the workflow file alone.

**Without `gh`:** after the local verification is green,
`git checkout main && git pull --ff-only && git merge --no-ff
task/pP-NN-<name> -m "Merge task/pP-NN-<name>" && git push && git branch -d
task/pP-NN-<name>`. CI runs on `main`; the reviewer watches it.

The driver considers the task done when `tasks/phase-P/reports/NN.md` exists
on `origin/main`. Nothing else counts. Merging never releases: a release is a
person pushing a tag `vX.Y.Z` (or pressing *Run workflow* for a dry run).

## 5. Stop conditions

Stop — do not merge, do not continue — when: CI is red a second time (§4); an
acceptance criterion cannot be met; a step needs a secret or a human (a board
on a bench, a GitHub secret, the release key); neither a compiler nor Docker
is there for `pio test -e native`; **the design the task rests on turns out
not to hold** (phase 1's task 01 exists to find that out — say what does not
hold and what would). Then: leave the branch pushed (PR open if `gh` exists),
write `tasks/phase-P/logs/STOPPED-NN.md` (git-ignored) saying exactly what is
missing and what was completed, and end. The driver stops the run and the
human reads that file.

## Phase-specific

**Phase 0.** Task 01 is the toolchain and everything after it inherits its
decisions: how MeshCore is consumed, how the board is named, what
`tools/verify.ps1` runs. Its report is required reading for every later task.

**Phase 1.** Task 01 is the design's proof (`spec/SYSTEM.md` S-1, S-2): our
application as a subclass of upstream's unmodified repeater. If it cannot be
built that way, that task stops and says so; nothing after it should be
attempted on a different design without the reviewer.

**Phase 2.** The mesh client is a port of `src/mesh/AgentMesh.cpp` of
twinoak/meshmaster-agent-firmware, read at the commit the task names. Its
rules are the rails of that repository's README and of `spec/SYSTEM.md` S-12:
one packet per send, a password in RAM for the length of the send, nothing
from a stranger decrypted, two-byte path hashes. Each has a test or a grep.

**The sync phase** (`-Phase sync`, once phase 0's task 03 has written it). A
task under `tasks/sync/` is a MeshCore bump that CI could not swallow.
`docs/upstream.md` is the page; the branch is `upstream/<tag>` and it already
exists — check it out rather than branching from the trunk.
