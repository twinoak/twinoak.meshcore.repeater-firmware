# 02 — Floods as the advert goes

**Depends on:** 01
**Spec:** `spec/SYSTEM.md` S-2 with its second exception (2026-11-14), and §10's point 4; `spec/LINK-PROTOCOL.md` §6.5 (*The rule*); upstream at the pin: `examples/simple_repeater/MyMesh.h` (`region_map` ~L94 and `default_scope` ~L97, both private; `sendFloodScoped` ~L198, public) and `MyMesh.cpp` (`sendFloodScoped` ~L999, `sendSelfAdvertisement` ~L1033, the default scope set in `begin` ~L955 and in `onDefaultRegionChanged` ~L1158), `src/helpers/RegionMap.h`, `src/helpers/TransportKeyStore.cpp`; `src/common/cxx/PrivateMember.h` and its one user, `src/roles/repeater/link/LocalRequest.cpp`; `src/roles/repeater/app/TwinOakMesh.cpp` (`dispatch`, ~L218)
**Size:** one session.
**Model:** claude-opus-5

## Goal

Upstream sends this node's flooded advert with
`sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1)`. With a default
scope the packet carries a transport code made from the region's key and the packet; with none,
`sendFloodScoped` is plain `sendFlood`. The mesh client's floods leave through
`TwinOakMesh::dispatch`, which calls `sendFlood(pkt, 0, kFloodPathHashSize)` and nothing else.

After this task a client's flood that is to be scoped (task 01 decides which) takes the advert's
road: upstream's function, upstream's key, our two-byte path hashes.

## Deliverables

1. **`dispatch`.** A flood the route says is scoped goes out through upstream's
   `sendFloodScoped` with upstream's own `default_scope`; one that is not goes out through
   `sendFlood`, as today. No key is derived here, no transport code is computed here, and no copy
   of the scope is kept: it is read at each send, so a `region default` that changed is in force
   for the next one. Both roads pass `kFloodPathHashSize` (S-12).
2. **Reaching what is private.** `default_scope` and `region_map` are private at the pin. S-2's
   second exception allows both, read and never written. Reach them as `LocalRequest.cpp`
   reaches `handleRequest` (`PrivateMember.h`), keep their names in one file, and list them in
   `docs/upstream.md` beside the first two as places a pin bump re-reads. The request to upstream
   that pile 3.9 drafted — `protected:` moved up a line — would cover these as well; say so
   where that draft lives.
3. **The scope's name**, for `FLOOD_SCOPE`'s answer: the default region's, from upstream's
   `region_map`, without a leading `#`; empty when there is none. **Not by running
   `region default` through `handleCommand`.** While a `region load` is in progress upstream
   reads every line as a region's line (`region_load_active`), and a `FLOOD_SCOPE` that arrived
   then would add a region to somebody's half-loaded map.
4. **What `scoped` answers.** 1 when the flood went through `sendFloodScoped` with a key that is
   not null (`TransportKey::isNull`), because the key is what was on the air. Read what upstream
   does at the pin when a default region's keys cannot be loaded (a private `$` region:
   `TransportKeyStore::loadKeysFor`, and the return `onDefaultRegionChanged` does not read) and
   say in the report whether a name without a key or a key without that name can exist, and what
   `FLOOD_SCOPE` and `scoped` answer in each case. Change nothing of upstream's behaviour.
5. **The board has the interface** of task 01, and with it the bit: `HELLO`'s `caps` and
   `EV_BOOT`'s carry `0x100`.
6. **Proof without a radio, as far as it goes.** A pin that renames or retypes either member is
   a build error, not a surprise on the air; show that it is. A rail, where the repository keeps
   its rails, that upstream's two names appear in one file only, and that the client's
   `sendFlood` call exists once, on the unscoped road.
7. **The bench**, as lines in `docs/bench.md` for a person with the unit, a Walter of link 1.3
   and a second repeater that logs what it hears: the unit's default scope set to `dk`; a login
   to a repeater it holds no path for; the first packet seen as a transport flood and forwarded
   by a repeater that holds `dk`, the second as a plain flood; `rak scope` on the Walter. Not run
   by this session.
8. **The words.** `docs/provisioning.md` where it sets `region default dk`: from link 1.3 that
   is also the scope the unit's logins flood in. A line in `release-notes/unreleased/` for this
   task.

## Acceptance criteria

- [ ] The board's build answers `HELLO` with `0x100` in `caps` (say how it was checked without a board).
- [ ] `default_scope` and `region_map` are named in one file, and `docs/upstream.md` lists both (rail or grep, in the report).
- [ ] The client calls `sendFlood` in one place, for the unscoped road, and `sendFloodScoped` in one place, with `kFloodPathHashSize` on both (rail or grep).
- [ ] With no default scope the packet is what it was before this pile: `sendFloodScoped` with a null key is upstream's `sendFlood` (cite the lines at the pin), and `scoped` answers 0.
- [ ] Nothing under upstream's tree or `spec/` was edited; `python spec/tools/specsync.py verify spec` passes.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The Walter's half: its pile 14. A scope of the client's own, different from the node's. Asking
upstream for anything beyond the draft that exists. The board: no session opens a port. A
release: Thomas presses the button.
