# 05 — Local management

**Depends on:** 03 (and runs after 04).
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.3, §5.7, §8, §9; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2, §4.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The box beside this one can manage it without the radio: any console command,
over the link, with console privileges. And this firmware can ask for the
Walter's attention — because a person on the mesh said so, or (from task 06)
because the battery is going — and knows to keep quiet while the Walter sleeps.

## Read first, in the pinned checkout

`MyMesh::handleCommand` and `CommonCLI::handleCommand` (the commands that need
`sender_timestamp == 0`, the size of the reply buffer, the commands that
restart and never return), and the `sensor get/set/list` branch with
`SensorManager`'s settings methods.

## Deliverables

- **`CLI`** (§5.7): the request's line run through
  `the_mesh.handleCommand(0, line, reply)` — timestamp 0 is what makes it a
  console command — and the reply returned. A line of 0 or more than 160
  bytes, or one that is not valid UTF-8 text without control characters, is
  `ERR_MALFORMED`. The reply buffer is sized and guarded as upstream's own
  console loop sizes it; if a command can write more than fits, the report
  says which and the handler truncates rather than overruns. Capability bit
  `CLI`. For a line that restarts the node, the response is not owed: the
  handler does not try to out-run the restart.
- **Our settings**, complete (ARCHITECTURE §4): `walter.wake`, `walter.seen`,
  `link.errors`, `clock.state`, `fw.version` — listed by `sensor list`, read
  by `sensor get`, and `sensor set walter.wake 1` accepted from the console
  **and from a mesh admin** (it goes through upstream's own CLI, so upstream's
  own permission check is the gate — confirm that from the source and say so).
  Upstream's own sensor settings, if the manager has any on this board, still
  work: ours are added, not substituted.
- **Attention** (`src/link/attention/`, the machine portable) — §8:
  - reasons 1 to 6, one pending at a time with a small queue behind it; a
    reason already pending or queued is not added twice;
  - to raise: `0x00 0x00`, 300 ms, `EV_ATTENTION` with `repeat` 0; then every
    1000 ms with `repeat` counting up, at most 10 sends; **any valid request**
    ends it;
  - a rate limit of one *new* attention per reason per hour, counted from when
    it was raised, kept in RAM;
  - the pending reason is in `STATUS.attention` until a valid request has
    arrived after it was raised, whether or not the sends were used up.
  - Raised here by `walter.wake` (reason 4) and by a restart (reason 1, sent
    after `EV_BOOT` **only if** the Walter was believed asleep in the surviving
    RAM of task 04 — otherwise `EV_BOOT` alone is enough). Tasks 06 and phase
    2 raise the others through the same call.
- **`SLEEP_NOTICE`** (§8): sets "the Walter is asleep" (`STATUS` flag bit 2);
  any valid request clears it. While it is set the session sends no events but
  `EV_BOOT` and `EV_ATTENTION`. Capability bit `ATTENTION`.
- **`linkcli.py`**: `cli "<line>"`, `sleep [seconds]`, and `listen` printing
  attentions with their reason in words.
- **`docs/link.md`**: the settings table, the attention reasons, and a session
  showing a mesh admin's `sensor set walter.wake 1` arriving as an attention.

## Tests (native)

- `CLI` through the session against a fake command interpreter: the fixture
  cases byte for byte; the empty reply; the over-long line; a control
  character; a reply at the buffer's limit.
- The settings: `sensor list` order is stable, unknown key, read-only key set.
- The attention machine against a fake clock: the 300 ms gap, ten sends a
  second apart with `repeat` 0–9, ended early by a request, the hourly limit
  per reason, two reasons queued, `STATUS.attention` before and after.
- `SLEEP_NOTICE`: events held while asleep, `EV_ATTENTION` not held, the flag
  cleared by the next request.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The report names, from the source at the pin, every console command that never returns and what the link does for each.
- [ ] The report confirms from the source who may run `sensor set` over the mesh.
- [ ] A grep in the rails test: nothing under `src/link/` logs a `CLI` line or its reply (a `password …` line is among them).

## Not verified here (Thomas, with a board)

Add to `docs/bench.md`, "4. Management over the link":

- `linkcli.py cli "get radio"`, `cli "neighbors"`, `cli "stats-core"` answer as
  the USB console does; `cli "set name Bench"` changes the advert name.
- `linkcli.py sleep 600`, then from a phone logged in as admin over the mesh:
  `sensor set walter.wake 1` — `listen` shows `EV_ATTENTION` reason 4 ten
  times a second apart, and stops at the next request.
- A second `walter.wake` inside the hour raises nothing.

## Out of scope

Power and environment and the battery attentions (06). What the Walter does
when woken (its repository).
