# Phase 3.12 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] `sensor set walter.wake 1` answers `ok` whatever happened.** `Settings::set`
  calls `raiseWalterWake()` and returns true without looking at what it returned
  (`src/roles/repeater/app/Settings.cpp`); `AttentionMachine::raise` refuses when the same reason
  is pending or was raised less than an hour ago. On 13 November a wake request was answered
  `ok` (meshmaster.dk job 28399) and the unit did not check in, and nothing on the server can say
  whether an attention was raised. The answer could say so.
- [ ] **[later] The radio cannot say how many attentions it raised, or how many the hourly limit
  refused.** `AttentionMachine` counts both (`raised()`, `rateLimited()`); `STATUS` carries
  neither. The Walter's pile 13 has the same follow-up from its side; the link protocol is the
  design repository's.
- [ ] **[Thomas to decide] `main`'s required check and the notes' pull request.** While `main`
  requires `verify`, every release leaves one pull request to merge by hand. meshmaster.dk's
  merges itself because its `master` requires a pull request and no check.

## Review of 01 (14 November)

Read: the branch `task/p3.12-01-humidity-and-pressure` (3804cb8, e125b55; pull request #51, open),
its report, the three source files it changes, `test/test_cayennelpp`, `docs/link.md` §2d, and
`logs/STOPPED-01.md`. **The work is done and right; it is not merged, because no CI run can start.**

- The encoding is the library's, held two ways: `static_assert`s against the library's own
  constants where a board build compiles them, and a native suite that links CayenneLPP 1.6.1 and
  compares the mirror with it field by field. Types 104 and 115, one and two bytes, half a per
  cent and a tenth of a hectopascal.
- Checked outside this repository: both copies of the agent library (`ReplyCodec.cpp` in the
  exit-node firmware and in the Walter's `components/agent-core`) and meshmaster.dk's
  `CayenneLpp.cs` decode 104 and 115 at the same size and scale. A reader that did not know 115
  would have stopped before the totals; none does.
- The reply is 97 bytes of Cayenne-LPP where v0.1.4 sent 90, by arithmetic and not by a radio.
- The session's own check of the runners was the finding of the day (below).

- [ ] **[03] No job of this repository has had a runner since 13 November.** The organisation's
  runner group *Default* is *All repositories, excluding public repositories* (five runners,
  unraid-1 to 5), and this repository is public. `ci.yml` and `upstream.yml` ask for
  `[self-hosted, unraid]`. The exit-node firmware and the Walter's, public too, run their CI on
  `ubuntu-latest`. Task 03 was written for it on the day and runs first.
- [ ] **[later] `STOPPED-01.md` says `release-notes/v0.1.4`'s pull request is still waiting.** It
  is not: #50 was merged by hand on 13 November (00472a7). Nothing follows from it.
- [ ] **[later] Seen on meshmaster.dk, after the release.** Channel 5 of the unit's repeater
  should show three readings on its Monitoring tab and three charts. Nobody has looked yet,
  because nothing is released.
- [ ] **[later] What I told Thomas on 13 November was wrong.** I read the queued run on `main`
  (43478441183) as waiting behind meshmaster.dk's pile on a busy runner. It was waiting for a
  runner this repository may no longer use.
- [ ] **[Thomas to decide] The mesh client's own floods carry no scope.** Seen on 14 November,
  when Thomas asked why the unit at Bellevue (repeater 41 on meshmaster.dk) seemed to advert
  without one. Its adverts are upstream's and take the default scope
  (`examples/simple_repeater/MyMesh.cpp:1037` and `:1299` at the pin; `region default dk` was
  answered ` default scope is now dk` that morning and read back after a restart). What leaves
  this firmware unscoped is every flood the mesh client sends for the Walter:
  `TwinOakMesh::dispatch` calls `sendFlood(pkt, 0, kFloodPathHashSize)`
  (`src/roles/repeater/app/TwinOakMesh.cpp:231`), and neither the client nor
  `spec/LINK-PROTOCOL.md` has a word for a scope. `spec/SYSTEM.md`'s open point 4 says these
  floods "follow the agent firmware (unscoped)". The agent firmware has changed since: the agent
  library's `FloodScope` floods the scope first and keeps one unscoped try (meshmaster.dk D-47),
  and by that file's own comment a repeater at `flood.max.unscoped 0` drops an unscoped flood at
  the first hop. The Walter's copy of the library has `FloodScope` too; what its side of the link
  does with it was not read.
