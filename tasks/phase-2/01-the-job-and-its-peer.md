# 01 — The job and its peer

**Depends on:** phase 1.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §6 (the rules at its head), §6.1; [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-1, S-4, S-12; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2 ("an extra peer in front"), §7.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The mesh client's state, and the one place it touches upstream's packet
handling. After this task the Walter can name a peer and a path, and
`TwinOakMesh` will decrypt that peer's replies to us — and only that peer's,
and only while a reply is awaited — without disturbing a single packet of the
repeater's own business. Nothing is transmitted yet; task 02 sends.

## Read first

- `src/mesh/AgentMesh.{h,cpp}` of the agent firmware at
  `51faafb427cba41b0de9983736b6621bc155f829`: `beginJob`, `endJob`,
  `setDirectPath`, `forgetPath`, `describePath`, `sharedSecret`,
  `searchPeersByHash`, `getPeerSharedSecret`, and the header's opening comment.
- In the pinned MeshCore: `Mesh::onRecvPacket` (`src/Mesh.cpp`) — how the
  match count, `getPeerSharedSecret(j)` and the MAC check are used, and the
  comment about how many matches it supports; `MyMesh::searchPeersByHash`,
  `getPeerSharedSecret`, `onPeerDataRecv`, `onPeerPathRecv`
  (`examples/simple_repeater/MyMesh.cpp`); `Packet::isValidPathLen` and the
  path-length encoding.

## Deliverables

- **`src/meshclient/Job.{h,cpp}`** (portable): the peer's key; the path — none,
  direct, or hops with a hash size, held in MeshCore's own encoding (the
  `path_len` byte, `(size − 1) << 6 | count`) and validated exactly as
  LINK-PROTOCOL §6.1 bounds it; "a reply is awaited" with its kind, tag and
  deadline (task 02 arms it; this task defines it and its clearing); and the
  rule that ends a job after 120 seconds of link silence.
- **`PeerSlot`** (portable): the index arithmetic of ARCHITECTURE §2 as a pure
  function — given upstream's match count and whether the extra peer matches,
  the total; given an index, whether it is ours or which of upstream's it is.
  It is four lines and it is the part that must never be wrong, so it is
  separate and exhaustively tested.
- **The overrides in `TwinOakMesh`**, replacing task p1-01's pass-throughs:
  - `searchPeersByHash`: upstream's count; plus one, in front, when a reply is
    awaited **and** the job's peer matches the hash. With no reply awaited the
    function is upstream's result and nothing else — say so in a comment,
    because that sentence is the proof that the repeater is undisturbed.
  - `getPeerSharedSecret`: ours for our index (the ECDH secret calculated once
    per job, on first need), upstream's for the rest with the index shifted.
  - `onPeerDataRecv`, `onPeerPathRecv`: ours for our index — for now a counter
    and nothing else — upstream's for the rest, shifted.
  - Check upstream's limit on the number of matches and that one more cannot
    exceed it or overrun its array; cite the lines.
- **The shared secret is wiped**, not dropped, when a job ends — as the agent
  firmware wipes it, through a volatile pointer so the compiler cannot elide
  it.
- **The link**: `JOB_BEGIN`, `JOB_END`, `PATH_SET`, `PATH_GET` (§6.1), the
  capability bit `MESH_CLIENT`, `STATUS` flag bit 0, and the 120-second rule.
  A second `JOB_BEGIN` ends the first job. `PATH_SET` with no job is
  `ERR_STATE`. A path that is not legal is `ERR_MALFORMED` and changes
  nothing.
- **`linkcli.py`**: `job begin <key> [--direct | --path aabb,ccdd]`, `job end`,
  `path`.

## Tests (native)

- `PeerSlot` for every combination of upstream count 0–4 and extra yes/no:
  totals, which index is ours, the shift.
- `Job`: the three path modes; hash sizes 1–3; 63 hops; 64 bytes; one byte too
  many; a hop count that disagrees with the bytes; the encoding byte for each.
- The fixture cases of the four requests through the session, byte for byte.
- The 120-second rule against a fake clock: silence ends the job; any request
  on the link restarts the count.
- The wipe: after `JOB_END` the buffer that held the secret is zero (read it
  back in the test, as the agent firmware's credential test does).

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The report quotes the upstream lines that bound the number of matches and shows the extra peer stays inside them.
- [ ] With no job, or a job with no reply awaited, the three overrides are provably upstream's: the code path contains nothing else (a reviewer can read it in ten lines).
- [ ] The comment above each override cites the upstream function and the pin.

## Not verified here (Thomas, with a board)

Add to `docs/bench.md`, "6. The mesh client":

- With a job begun and nothing sent, the repeater behaves exactly as before: a
  phone logs in as admin, runs a CLI command, reads status; messages pass
  through. (The extra peer is only there while a reply is awaited, which this
  task cannot yet cause.)

## Out of scope

Sending anything; arming a wait; taking a reply (02). Observations (03).
