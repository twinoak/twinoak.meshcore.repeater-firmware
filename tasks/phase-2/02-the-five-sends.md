# 02 — The five sends

**Depends on:** 01.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §6 (all of it); [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-4, S-12; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §7.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The Walter can make the five exchanges an exit node makes with a repeater —
a login, a request, a CLI line, a path discovery, a trace — and get the reply.
Each send is **one packet**, built with MeshCore's own primitives exactly as
the agent firmware builds it, and the reply is taken only from the job's peer
and only while it is awaited.

## Read first

- `src/mesh/AgentMesh.cpp` of the agent firmware at
  `51faafb427cba41b0de9983736b6621bc155f829`, every function from `dispatch` to
  `onTraceRecv`, **with its comments**: why a login's tag is 0; why the
  password is at most fifteen bytes; why a CLI line's timestamp matters and
  where it comes from; why discovery always floods; why a trace cannot carry
  three-byte hashes; why no reciprocal path is sent. Each of those was learned
  on the air. Also `src/agent/Credentials.{h,cpp}` (`wipeBytes`) and
  `src/mesh/ReplyCodec.h` (`parseCliReply`).
- In the pinned MeshCore, the functions that file cites: `BaseChatMesh::sendLogin`
  and `sendMessage`, `MyMesh::handleLoginReq` and `onPeerDataRecv` on the
  *receiving* repeater, `Mesh::createAnonDatagram`, `createDatagram`,
  `createTrace`, `sendDirect`, `sendFlood`, and the trace branch of
  `Mesh::onRecvPacket`. Check each claim against the pin; cite what you rely
  on.

## Deliverables

- **`src/meshclient/Reply.{h,cpp}`** (portable): the reply record of §6.3 and
  the matching rules as pure functions — what is awaited (a response, a text,
  a trace), by which tag (0 = the next from the peer), until when; a response
  while a text is awaited is dropped; a response with another tag is dropped;
  a text that is not CLI data is dropped; the timeout. One record is held from
  its arrival until the next send, `JOB_BEGIN` or `JOB_END`.
- **The sends, in `TwinOakMesh`** — each a port of the agent firmware's:
  `SEND_LOGIN`, `SEND_REQUEST`, `SEND_CLI`, `SEND_DISCOVERY`, `SEND_TRACE`.
  For each:
  - refuse with `ERR_STATE` without a job (a trace needs none), with
    `ERR_BUSY` while a reply is awaited, with `ERR_RANGE` / `ERR_MALFORMED`
    for what cannot be sent (a password over 15 bytes, a line over 148, a
    trace with three-byte hashes);
  - take the timestamp from `getRTCClock()->getCurrentTimeUnique()`, at the
    moment the packet is built;
  - build **one** packet; send it direct along the job's path (modes direct
    and hops) or as a flood with **two-byte path hashes** when there is none;
    discovery always floods, a trace is always direct;
  - **wipe** every buffer that held a password — the request's payload in the
    link's receive buffer included, and the remembered response of the session
    must not contain it (it does not: the response carries no password; check
    that the *request* is not what is remembered) — before returning;
  - arm the wait in the same step when `timeout_ms` is not 0, and answer the
    link with `send`, `tag`, `sender_ts` (§6.2);
  - count it in `client_tx`, once.
- **Taking the reply** — the bodies of the overrides task 01 left empty:
  `onPeerDataRecv` for a `RESPONSE` and for a `TXT_MSG` of CLI data;
  `onPeerPathRecv` learning the returned path into the job and taking the
  response that rides in it, **returning false** (no reciprocal path: one
  packet per send); `onTraceRecv` for a completed trace with the awaited tag.
  SNR from the packet, RSSI from the note `logRx` took (task 03 adds the note;
  until then `0x7FFF`).
- **Reporting it**: one `EV_MESH_REPLY` when the reply arrives or the wait
  times out (not sent while the Walter is believed asleep — it is awake, it
  just asked; but the rule of task p1-05 stands and `REPLY_GET` is the
  fallback), and `REPLY_GET` at any time. `STATUS` flag bit 1.
- **The mesh loop is never blocked.** There is no `await()` here: the wait is
  a deadline checked from `loop()`.
- **`MESH_COUNTERS`** (§6.4): `rx` and `tx` from the radio driver's own
  counters, `client_tx` ours, the rest zero until task 03, noise floor and last
  RSSI/SNR from the radio.
- **`linkcli.py`**: `login [password]` (read from a prompt, not the command
  line, when given as `-`), `request <type> [hex]`, `status-of-peer` and
  `neighbours-of-peer` as conveniences that decode the two commonest replies,
  `send-cli "<line>"`, `discover`, `trace <hashes>`, `reply`, `counters`.
  It prints a password as stars.

## Tests (native)

- `Reply`: every matching rule above against scripted arrivals — the right
  response; a response with another tag; a response while a text is awaited;
  a text while a response is awaited; a late reply after the timeout; two
  replies (the first wins, the second is dropped); the timeout itself.
- The fixture cases of the five sends, `REPLY_GET` and `EV_MESH_REPLY` through
  the session against a fake mesh, byte for byte.
- `ERR_BUSY` while awaiting; the same request repeated on the link (same
  `seq`) causes **one** call into the mesh and two identical responses.
- The wipe: after `SEND_LOGIN` and after a sensitive `SEND_CLI`, no buffer the
  session or the client owns still holds the password bytes (search them).
- A rails-test line: nothing under `src/meshclient/` calls a logging or print
  function with packet bytes.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] For each of the five sends the report gives the upstream functions and lines it was checked against, and any place where the pin differs from what the agent firmware's comments say.
- [ ] `client_tx` rises by exactly one per send in the tests, and by zero for a refused or repeated request.
- [ ] No function in the send or reply path can block: no loop that waits on the radio or the clock.

## Not verified here (Thomas, with two radios)

Add to `docs/bench.md`, "6. The mesh client", with a second repeater (any stock
MeshCore repeater on the bench channel) and a companion as an observer:

- `job begin <its key>`, `login` (blank): answered at guest if it has no guest
  password, silence if it has one — both are right, and `reply` says which.
- `login -` with its admin password: answered; `status-of-peer`,
  `neighbours-of-peer`, `send-cli "get name"` each answered; `path` now shows
  the route the first reply came back by.
- The observer counts **one packet per send** from this unit, and a login's
  packet is not readable as a password.
- `trace` along the learned path comes back with an SNR per hop.
- While all of this runs, a message through the unit still arrives, and a
  phone can still log in to the unit itself.
- `job begin` with the key of a repeater that is also an admin of this unit:
  its CLI reply is read as a reply and nothing is executed on this unit
  (ARCHITECTURE §2, the reason the peer is in front).

## Out of scope

The job logic above the sends — sessions, classes, the ACL restore, retries:
that is the agent's, on the Walter (`spec/proposals/03`). Observations (03).
A cap on how much the client may send (04).
