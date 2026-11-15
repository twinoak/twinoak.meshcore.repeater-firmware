# Phase 3.13 — the scope a unit's logins go out in

Written on 14 November.

**What happened.** Thomas asked where `region default dk` is set, because the unit at Bellevue
seemed to advert without a scope. Its adverts are upstream's and are scoped
(`MyMesh::sendSelfAdvertisement`, through `sendFloodScoped`). What is not is every flood the mesh
client sends for the Walter: `TwinOakMesh::dispatch` calls plain `sendFlood`. `spec/SYSTEM.md`'s
open point 4 said those floods follow the agent firmware, unscoped, until the Danish network's use
of scopes says otherwise. The agent firmware has flooded its scope first, with one try without it,
since meshmaster.dk's D-47.

**What Thomas decided.** Jobs sent through a Walter carry the default scope too. Open point 4 is
closed.

**What it takes.** The link is version 1.3 (`spec/LINK-PROTOCOL.md` §6.5, copied here with
`link-v1.3.json` before this pile). In a session of 1.3 the client's floods go out exactly as the
node's own flooded advert does; `FLOOD_SCOPE` reads the scope's name and asks for one send
without it; and a send's answer says whether it went out scoped. A session of 1.2 is what it was,
to the byte and to the packet.

**What this pile builds.** The radio's half, in two steps: the link's side against a fake (01),
then the board's side, which is upstream's own function with upstream's own key (02). The
Walter's half is its pile 14. Either may be released first.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [the-link-speaks-of-scope](01-the-link-speaks-of-scope.md) | — | link 1.3 in the codec, the session and the mesh client: the session's version remembered, `FLOOD_SCOPE`, one send without the scope, a send's `scoped`; behind an interface no board implements yet, so nothing changes on the air |
| 02 | [floods-as-the-advert-goes](02-floods-as-the-advert-goes.md) | 01 | the board's side: the client's floods through upstream's `sendFloodScoped` with upstream's default scope, the scope's name from upstream's region map, the capability in `HELLO` |

```powershell
.\tasks\run-all.ps1 -Phase '3.13'
```

## Models

- **Opus**: 01, 02.

## Afterwards (Thomas)

- **Release** with the button, merge its notes pull request, then offer and apply the radio's
  firmware to the unit from meshmaster.dk.
- **The Walter's half**: its pile 14, released, offered and applied. Until both are on the unit
  nothing changes on the air.
- **The bench, when there is time:** task 02's lines in `docs/bench.md`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
