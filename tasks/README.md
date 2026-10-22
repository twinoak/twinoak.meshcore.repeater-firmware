# Tasks — how the firmware gets built

## Where things are decided

| | Where | In this repository |
|---|---|---|
| what a unit is, the decisions (`S-1`…) | `twinoak.meshcore.standard-repeater-design-v2/software/spec/SYSTEM.md` | a copy, `spec/SYSTEM.md` |
| the link to the Walter | …`/LINK-PROTOCOL.md` + fixtures | a copy, `spec/`; spoken by `src/link/` |
| the release manifest and the image descriptor | …`/FIRMWARE-MANIFEST.md` + fixtures | a copy, `spec/`; written by the release tooling |
| the pins and the boards | …`/hardware/pcb/INTERCONNECT.md` (**frozen**) | read from there, by link, at a commit |
| MeshCore | `meshcore-dev/MeshCore` | a dependency, pinned (`docs/upstream.md`) |
| the mesh client's model | `twinoak/meshmaster-agent-firmware`, `src/mesh/AgentMesh.*` | ported, at a commit the task names |
| how this firmware is put together | — | `docs/ARCHITECTURE.md` |

`spec/` is copied, never edited: `python spec/tools/specsync.py verify spec`
holds every file against `spec/MANIFEST.txt`, in `tools/verify.ps1` and in CI.
When a task finds the specification wrong or silent, it says so in its report
and takes the smallest decision that fits; the reviewer changes the
specification where it lives and copies it again.

## Conventions

- **One task, one branch, one PR.** Branch `task/p1-02-the-link-codec` for
  `tasks/phase-1/02-the-link-codec.md`. The PR description is the task's
  acceptance checklist, ticked.
- **Read before writing.** Every task lists what it implements. Read those
  sections in full before touching code.
- **Read the firmware you build against.** MeshCore is on disk after a build,
  under `.pio/libdeps/<env>/MeshCore`, at the commit `[meshcore].pin` names. A
  claim about what MeshCore does — in a task file too — is checked against
  that source, never against a memory of it. Where code depends on a line of
  upstream's, a comment cites the file, the function and the pin.
- **Not one file of upstream's is edited.** Our application subclasses and
  composes. If something cannot be done that way, that is a finding for the
  report, not a patch.
- **Verification is part of the task, and it is local.** A task is done when
  `tools/verify.ps1` and the task's own commands pass on this machine, and
  then in CI. `-Werror` on our own sources means a warning is a failure.
- **Logic is portable; the part that touches the board is thin.** Anything
  that can be a pure function or a class behind an interface lives in files
  with no Arduino and no MeshCore include, compiled by `[env:native]` beside
  its tests. A task that adds logic adds its native tests.
- **No scope creep.** If a task needs something a later task owns, stub it
  behind a seam and say so in the PR.
- **Small, deliberate dependencies.** Libraries are pinned to an exact version
  in `platformio.ini`; adding one needs a one-line justification in the PR.
- **Secrets never in the repo**, and no default password in an image
  (`spec/SYSTEM.md` S-13).
- **Docs move with code.** A task that changes what the firmware says on its
  console, what the link carries or how it is flashed updates `docs/` in the
  same PR, and adds its bench lines to `docs/bench.md`.
- **Leave the evidence.** Build logs, images and test output stay on disk in
  `out/` after the run (git-ignored), so a reviewer can inspect them instead
  of trusting the PR text.

## Unattended runs

`tasks/run-all.ps1 -Phase N` runs the tasks of a phase in order, each in a
fresh headless session (`claude -p --dangerously-skip-permissions`) that
follows [`RUNBOOK.md`](RUNBOOK.md): branch, implement, verify locally, write
`phase-N/reports/NN.md`, open a PR, wait for CI, merge. A task counts as done
when its report exists on `origin/main`; the first task that does not land
stops the run (`phase-N/logs/STOPPED.md`), and `-From NN` resumes.
`tasks/run-pile.ps1` runs phase after phase and stops where a phase stops.
Review findings go into `phase-N/FOLLOWUPS.md`, tagged with the task that must
pick them up. Merging never releases; a release is a person pushing a tag.

## The piles

| Phase | What | Tasks |
|---|---|---|
| [0](phase-0/README.md) | the repository: toolchain, CI, release, the upstream watch | 3 |
| [1](phase-1/README.md) | a repeater that is ours: the application, the link, the clock, local management, power | 6 |
| [2](phase-2/README.md) | the exit-node half: the mesh client, observations, the rails | 4 |
| [3](phase-3/README.md) | the family: roles and boards, NoiseScope, first-boot provisioning | 3 |
| [3.1](phase-3.1/README.md) | the fast sweep: the in-radio sweep of MeshCore #3578 as the scanner's `scope`, 150–960 MHz, a live waterfall | 1 |
| [3.2](phase-3.2/README.md) | a MeshCore bump can be green: the version tests assert the defines, not a release | 1 |
| [3.3](phase-3.3/README.md) | the band's ceiling, and a bench a session may use: `tools/bench/`, `bench walk` | 3 |
| [3.4](phase-3.4/README.md) | the base in the release (`use: provision`, `spec/SYSTEM.md` S-5a), and the first board walked | 2 |
| [3.5](phase-3.5/README.md) | what the first board showed: a new unit on the bench, the scope across the band, a watchdog an update gets through, the clock and the thresholds walked, the Danish standard, MeshCore 1.18 | 6 |

Not in these piles, on purpose: anything that needs the rig (a bench pile
follows the first boards), the bridge role and the dual adapter (the structure
is ready for them; `docs/ARCHITECTURE.md` §7), and anything on the MeshMaster
side (`spec/proposals/`).
