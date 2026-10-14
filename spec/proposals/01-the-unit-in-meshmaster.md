# Proposal 01 — the unit in MeshMaster: a direct agent that is also an exit node

**For:** `twinoak/meshmaster.dk`. **Status:** accepted 2026-10-14 — option two of A (a `mesh` agent with `servesRepeaterId`); built as pile 4.14, tasks 119–121. Written 2026-10-11 while that repository
was closed for other work. Nothing in either firmware depends on it yet. Read against
`spec/PLUGINS.md` §3 and §6, `spec/EXIT-NODES.md` §2, §5, §9, §10 and D-26, at `01ac515`.

## What exists

The spec already describes the Walter as a **pack agent**: kind `direct`, transport `native`,
serving one repeater (`servesRepeaterId`), always the preferred candidate for that repeater when
online, with the mesh as fallback. `AgentKind.Direct` is in the core. Phase 5, which would have
built the TwinOak pack, was postponed by D-26 because no repeater ran the LTE design.

## What is new

1. **The unit should also be an exit node for its neighbours.** A repeater-v2 unit sits on a mast
   with an LTE uplink and a radio one hop from other repeaters. It can run the same jobs a Heltec
   exit node runs — against *other* repeaters — with no extra hardware.
2. **It is heard from rarely.** It runs on a 10 Ah battery and a 10 W panel; an LTE attach costs
   real energy. It will check in every fifteen minutes to several hours depending on the battery,
   not every 30–120 seconds.
3. **It has one key.** The radio that does the exit-node work is the repeater itself
   (`SYSTEM.md` S-1). The `companionPublicKey` its heartbeat reports is the public key of a
   repeater in the register.

## What it asks of MeshMaster

**A. One agent, two roles.** Either `direct` gains "may also serve as a mesh exit node", or a
`mesh`-kind agent gains `servesRepeaterId`. The second reads better against the code as it stands:
routing, budget, association and health are all written for `mesh` agents, and "this exit node is
bolted to repeater 17" is one nullable column and one routing rule (prefer it for 17, and for 17
send local job types instead of mesh ones). The transport word stays `native`.

**B. Health for an agent that sleeps.** `online` is "a heartbeat within 3× the poll interval"
today, against the interval the server last told the agent. For this agent the *agent* proposes
the interval (it knows its battery) and the server accepts it within bounds:

- the heartbeat carries `nextCheckInS`;
- health is judged against that: `online` within `nextCheckInS × 1.5 + a grace`, then `quiet`,
  then `offline`;
- the exit node's page says *asleep until about 14:30* rather than *quiet*.

**C. Jobs that wait for a check-in.** A job routed to this agent is not late because nobody has
claimed it for an hour.

- Expiry and lease for a job bound to a sleeping agent count from the claim, not from creation;
  a job created for it carries `notBefore`-style patience equal to the agent's interval.
- Routing: for a neighbour that an always-on exit node also reaches, prefer the always-on one;
  use the unit when it is the only one in reach, or when a person pins it.
- A claim returns enough for one wake: the server already groups by repeater; add a cap the agent
  states (`maxTransactionsThisWake`), because the unit's airtime and energy per wake are bounded.
- **A wake request.** For work that should not wait — a person pressed *Read now* — the server
  cannot reach a sleeping unit over LTE. It can over the mesh: a `cli` job through any other exit
  node that reaches the unit's repeater, sending the one command that raises the unit's attention
  (`sensor set walter.wake 1` on the repeater firmware). The page offers it as *wake it up*.

**D. One key in two tables.** Today a companion key that equals a registered repeater's key is not
expected. It should be recognised, not flagged: the exit node's page links to the repeater, the
repeater's page shows it has an uplink of its own, and an advert heard *from* that key is the unit
itself and never an observation about a stranger.

**E. Association when the exit node is a repeater.** The grant (`setperm <key> <level>`) puts the
*unit's repeater key* in a neighbour's ACL. That works on stock firmware — an ACL entry is a key —
but it reads oddly on the neighbour ("a repeater has read access to me"). Worth one sentence on the
association page, and worth checking that nothing in the flow assumes the granted key is a
companion's (the ACL restore after an admin session, §7, is the same either way).

**F. Local job types for its own repeater** — as PLUGINS §6 already lists: `status`,
`config.read`, `config.apply`, a console line, power, power-cycle, the OTA steps, a NoiseScope
session. Their shapes are proposal 02 and the pack; nothing here changes them.

## What it does not ask

No change to the agent protocol's endpoints or envelopes beyond the two optional fields above
(`nextCheckInS`, `maxTransactionsThisWake`). An agent that does not send them is an agent as today.

## Suggested order

1. A and B together: a `mesh` agent with `servesRepeaterId` and a self-declared interval. That is
   enough for a unit to appear, heartbeat and report observations.
2. C: patience in routing and expiry; the wake request.
3. D and E: the pages.
4. F with proposal 02: the pack.
