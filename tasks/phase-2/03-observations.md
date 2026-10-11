# 03 — Observations

**Depends on:** 01.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §3 (a repeated request), §6.4, §7, §8; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §7.
**Size:** one session.
**Model:** claude-opus-5

## Goal

Every advert this repeater hears — after its signature verified — is kept for
the Walter, with the path it arrived by and how well it was heard. That is the
free half of what an exit node tells MeshMaster: who is out there, and how they
are reached from here.

## Read first

- The agent firmware at `51faafb427cba41b0de9983736b6621bc155f829`:
  `AgentMesh::logRx`, `noteRssi` / `takeRssi` and the comment on why an RSSI
  has to be noted there; `AgentMesh::onAdvertRecv`; `src/agent/Observations.{h,cpp}`
  (the ring and its bounds).
- In the pinned MeshCore: `MyMesh::onAdvertRecv` and `logRx`, how
  `Dispatcher::checkRecv` can delay a flood packet before `onRecvPacket` sees
  it, `Packet::getPathHashSize` / `getPathHashCount` / `getPathByteLen`, and
  the size of the packet pool.

## Deliverables

- **`src/meshclient/ObservationRing.{h,cpp}`** (portable): a ring of **64**
  records of LINK-PROTOCOL §7 — key, the advert's own timestamp, when it was
  heard (as a monotonic time, so `age_s` is right whatever the clock says),
  SNR in quarter-decibels, RSSI or "unknown", the path-length byte and up to
  64 path bytes, up to 32 bytes of application data untouched. Full: the
  oldest is overwritten and a loss counted. State the RAM it costs.
- **Capture, in `TwinOakMesh`**:
  - `logRx`: call the base, then note the radio's last RSSI against the packet
    (the agent firmware's table, sized to this build's packet pool);
  - `onAdvertRecv`: **call the base first** — upstream's neighbour table is
    the repeater's own and must see every advert exactly as before — then push
    the record. An advert from this node's own key is not recorded.
  The advert's name is in the application data. It is copied, never parsed and
  never printed here.
- **`OBS_PULL`** (§7): up to `max` records, oldest first, as many as fit in
  one payload; `remaining`; `dropped` since the last pull. Records handed out
  leave the ring — and because the session remembers the response, a repeated
  request gets the same records again and the ring is not drained twice. Test
  exactly that. Capability bit `OBSERVATIONS`; `STATUS.obs_pending` and
  `obs_dropped`.
- **Attention reason 5** when the ring passes three quarters full while the
  Walter is believed asleep, through task p1-05's call and under its hourly
  limit. A unit on a busy mesh must not wake its Walter every hour for adverts
  alone: make the threshold a setting (`obs.wake`, default **off**), so that
  the default is "collect them when the Walter is awake anyway".
- **`MESH_COUNTERS`** completed: `adverts`, `adverts_dropped`, `obs_dropped`.
- **RSSI for replies**: the note taken in `logRx` now also fills the `rssi` of
  task 02's reply record.
- **`linkcli.py`**: `obs [max]`, printing key prefix, hops, SNR, RSSI and age —
  and the name only with `--names`, decoded from the application data on the
  laptop.

## Tests (native)

- The ring: fill, wrap, loss counted; pull fewer than there are, more than
  there are, zero; `age_s` across a clock change; a record with the longest
  path and the longest application data; a pull that can fit only one such
  record in a payload.
- The fixture cases of `OBS_PULL` through the session, byte for byte.
- A repeated `OBS_PULL` (same `seq`): the same records, the ring drained once.
- The RSSI note table: a pooled packet reused; a note taken back once.
- The attention threshold, off by default, and on.

## Acceptance criteria

- [ ] `tools/verify.ps1` green; the ring's RAM in the report.
- [ ] `onAdvertRecv` and `logRx` call their base before anything else, and the comments cite the upstream functions and the pin.
- [ ] A rails-test line: nothing under `src/meshclient/` prints or logs application data.

## Not verified here (Thomas, with a board on the roof antenna)

Add to `docs/bench.md`, "7. Observations":

- After ten minutes, `linkcli.py obs` lists the repeaters in range with
  plausible hops, SNR and RSSI; a direct neighbour has hops 0 and an RSSI.
- `neighbors` on the unit's own console still lists its neighbours as before.
- With the laptop away for a day, `status` shows the ring full and a loss
  count, and the repeater is none the worse.

## Out of scope

Turning observations into MeshMaster's JSON (the agent, on the Walter).
