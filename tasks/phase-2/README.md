# Phase 2 — the exit-node half

Four tasks that make this repeater the radio of a MeshMaster exit node: on the
Walter's behalf it logs in to a neighbouring repeater, asks it something, and
hands back the answer — as itself, with its own key (`spec/SYSTEM.md` S-1) —
and it keeps the adverts it hears for the Walter to collect.

The model is `src/mesh/AgentMesh.{h,cpp}` of twinoak/meshmaster-agent-firmware
at `51faafb427cba41b0de9983736b6621bc155f829`: a mesh client that has been on
the air against real repeaters, with its rules written in its comments. Tasks
01 and 02 are a port of that file onto `TwinOakMesh`. Read it in full, comments
first, before either; where this pile and that file disagree about what goes on
the air, that file is right and the report says so.

Two things differ from the Heltec, and both come from being a repeater:

- that firmware releases every packet that is not an advert or a job's reply
  before MeshCore sees it. This one cannot — it has a repeater's work to do —
  so the client's peer is added *in front of* upstream's own
  (`docs/ARCHITECTURE.md` §2) instead;
- that firmware blocks in `await()`. This one answers the link at once and
  reports the reply as an event.

## Order and dependencies

| # | Task | Depends on | Milestone |
|---|---|---|---|
| 01 | [the-job-and-its-peer](01-the-job-and-its-peer.md) | phase 1 | `JOB_BEGIN` / `JOB_END` / `PATH_SET` / `PATH_GET`; the extra peer in front of upstream's; nothing on the air yet |
| 02 | [the-five-sends](02-the-five-sends.md) | 01 | login, request, CLI line, discovery, trace; the reply as an event and on request; one packet per send |
| 03 | [observations](03-observations.md) | 01 | every verified advert into a ring; `OBS_PULL`; `MESH_COUNTERS` |
| 04 | [the-rails](04-the-rails.md) | 02, 03 | a backstop on what the client may send; every rule of S-12 with a test or a grep; the bench walk in one place |

01 → 02 → 03 → 04.

```powershell
.\tasks\run-all.ps1 -Phase 2
```

## Models

- **Fable** — 01, 02. What is decrypted for whom, where a password lives, and
  one packet per send are decided there.
- **Opus** — 03, 04.

## The exit criterion

From a laptop on the link: begin a job with a neighbouring repeater's key, log
in with its guest or admin password, read its status and its neighbours, send
it one CLI line, trace the path — and see, on a second radio, **exactly one
packet per send**. Meanwhile the unit repeats as before. Thomas's, with two
radios on the bench.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that
must pick them up.
