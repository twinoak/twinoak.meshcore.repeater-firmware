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
