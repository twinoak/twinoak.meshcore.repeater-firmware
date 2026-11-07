# Phase 3.9 — the key that was lost, and link 1.2

Written on the evening of 7 November. The first unit's radio lost its identity on 6 November.

**What happened, in order:**

1. A Walter at 0.1.0 tried a first install of repeater v0.1.0 over SWD. Its task watchdog reset it
   part-way through, twelve times, and it left the application half-written. The Walter's side is
   fixed in its v0.1.1.
2. Thomas recovered the radio over its own USB-C with v0.1.0's `.uf2`.
3. The radio's next `HELLO` carried a **new key**. meshmaster.dk's audit log has
   `agent.companion_changed` from `ebf91b805ca34a50…9773174` to `776cdcd5ca758256…f0ef7ac2`.
4. Its settings went with the key: the unit's repeater had to be registered again on meshmaster.dk
   (41, where it was 12), and its admin password became a random one nobody knows (S-13).

This firmware's `docs/flashing.md` (*A radio left half-written*, from pile 3.8) says the key
survives such a recovery. On the one radio it was tried on, it did not.

The radio was meant to keep its identity (Thomas, 5 November: the first radio keeps its key). That
identity is gone, because no copy of the private key existed. This pile finds what reset the node,
fixes it if it is here, and corrects the guide either way. Pile 3.8's review items for the same guide
come along.

**Link 1.2** rides along too. The same unit's repeater could not be read for its status or its
neighbours through its own Walter, because the link had no request for them (3.8's first follow-up).
Thomas settled it on 7 November: a new request answered with MeshCore's own bytes. The specification
is in `spec/LINK-PROTOCOL.md` §5.9 and `spec/fixtures/link/link-v1.2.json`, copied here on 7 November.
02 implements it, and with 02 there is always a release.

**Not on the board.** The radio is in use on Thomas's bench. No session opens its port.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-key-that-was-lost](01-the-key-that-was-lost.md) | — | the cause found from the code (or each candidate ruled out with its evidence), a fix where the cause is in this firmware, and *A radio left half-written* rewritten: the Walter's journal first, a recovery may give the radio a new key, how to tell, and what to re-apply |
| 02 | [the-local-request](02-the-local-request.md) | — | link 1.2: `LOCAL_REQUEST` answered from upstream's own `handleRequest`, upstream unedited; the codec and session at 1.2 with every rule tested; `docs/upstream.md` knows the new place; the request to upstream drafted |
| 03 | [the-release-0-1-2](03-the-release-0-1-2.md) | 01, 02 | `v0.1.2` published as *Latest* and verified; the notes say what 1.2 is for and which Walter speaks it |

```powershell
.\tasks\run-all.ps1 -Phase '3.9'
```

## Models

- **Opus**: 01, 02, 03.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md), tagged with the task that must pick them up.
