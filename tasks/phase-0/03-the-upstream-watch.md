# 03 — The upstream watch

**Depends on:** 01.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-2; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2 ("What breaks this, and how it is noticed").
**Size:** one session.
**Model:** claude-opus-5

## Goal

MeshCore moves, and this firmware is built on a pin. Keeping up is three tiers,
as in the agent firmware: a workflow that notices a new `repeater-v*` tag and
opens the bump as a pull request; one command that turns a bump CI could not
swallow into a task; and a page that says which is which.

## Read first

In twinoak/meshmaster-agent-firmware at
`51faafb427cba41b0de9983736b6621bc155f829`: `.github/workflows/upstream.yml`,
`docs/upstream.md`, `tools/upstream-check.ps1`, `tools/new-sync-task.ps1`,
`tasks/sync/TEMPLATE.md` and `tasks/sync/README.md`. This task is that set,
adapted — the tag family is `repeater-v*`, the trunk is `main`, and the list of
places a bump re-reads is this repository's.

## Deliverables

- **`.github/workflows/upstream.yml`** — weekly and on *Run workflow*. It reads
  the tags of `meshcore-dev/MeshCore`, and when the newest `repeater-vX.Y.Z` is
  newer than `[meshcore].tag` it pushes a branch `upstream/<tag>` that moves
  `pin` and `tag`, and opens a pull request *MeshCore <tag>* whose body lists
  upstream's commits between the two pins. CI runs on it like any pull
  request. It keeps the agent firmware's **rehearsal input** (`pin_tag`): the
  only way to exercise the "something newer" path while upstream's newest tag
  is our pin.
- **`tools/upstream-check.ps1`** and **`tools/new-sync-task.ps1`** — the
  comparison and the one command for a red bump, runnable by hand.
  `new-sync-task.ps1 -Tag repeater-v1.18.0 -Pr NN` writes
  `tasks/sync/NN-meshcore-<tag>.md` from the template and adds its row to
  `tasks/sync/README.md` in the shape the driver's regex reads.
- **`tasks/sync/TEMPLATE.md`** with this repository's standing instructions
  for a bump. The places a bump re-reads, each because it depends on upstream
  in a way a compiler may not catch:
  1. the flag blocks copied into `platformio.ini` from upstream's
     `[arduino_base]`, `[nrf52_base]`, `[sensor_base]` and
     `variants/rak4631/platformio.ini`;
  2. the exact versions of `[libs]` against upstream's ranges;
  3. `boards/rak4631.json` against upstream's;
  4. `tools/pio/*` — what they reach for inside the pinned tree;
  5. every comment in `src/` that cites a file and function of upstream's at
     the old pin (`grep` for the old pin's first eight characters);
  6. `build_as_lib.py`'s defines (`BUILD_EXAMPLE`, `EXCLUDE_FROM_EXAMPLE`,
     `MC_VARIANT`);
  7. the application region in the linker script (the release tooling's test
     catches a move; the manifest's `app_region` then changes, which the
     Walter must be told about — say so in the report).

  And the rails that must survive a bump: nothing the mesh says logged as
  text; not one upstream file edited; the native tests and the spec fixtures
  green.
- **`docs/upstream.md`** — the page: what is automatic, what is one command,
  and the thing that is neither — a change upstream makes to the *protocol*
  (packet layouts, the login, the CLI's replies), which reaches the mesh client
  of phase 2 and MeshMaster's own fixtures and is a reading task before it is
  a code change. It also records the pin's history, starting with today's.

## Acceptance criteria

- [ ] `tools/verify.ps1` green (nothing it runs changed).
- [ ] *Run workflow* against the current pin opens no pull request and says why; with `pin_tag=repeater-v1.17.0` it opens *MeshCore repeater-v1.17.1* on a rehearsal base branch, with the commit list, and CI builds it green. The rehearsal's pull request and both its branches are closed and deleted afterwards; the report links the runs.
- [ ] `tools/new-sync-task.ps1` writes a task file and a row the driver's regex (`^\| (\d\d) \| \[([a-z0-9-]+)\]\(`) accepts; the generated file is removed again and a copy left in `out/`.
- [ ] `docs/upstream.md` lists the seven places and is linked from `README.md`.

## Not verified here

The weekly schedule has not fired. A real bump has not happened: the first one
that does is the test of the template.

## Out of scope

Bumping the pin. Watching the agent firmware or the Walter library — those are
not this repository's dependencies.
