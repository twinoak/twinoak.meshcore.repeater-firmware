# Phase 3.13 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] The Walter's half is its pile 14.** Until a Walter of link 1.3 is on the unit the
  session is 1.2 and this firmware floods the client's packets unscoped, as before. That is
  correct, and worth knowing before somebody looks for a transport code on the air.
- [ ] **[later] The specification's source is not committed.** `spec/MANIFEST.txt` says *at commit
  204210b plus uncommitted changes*: links 1.1, 1.2 and 1.3 are in the working tree of
  `twinoak.meshcore.standard-repeater-design-v2/software/spec` and in no commit of it. Thomas's
  to commit there; the copy here is byte for byte what that tree holds.
- [ ] **[later] A default region whose keys cannot be loaded keeps the scope before it.** Read in
  upstream's code on 14 November, not seen on a board: `onDefaultRegionChanged` calls
  `getTransportKeysFor` and does not read what it returns, and for a private `$` region whose
  keys are not in the cache that is nought, with `default_scope` left as it was. Task 02 says
  what this firmware answers then. Upstream's to change.
- [ ] **[later] Every byte this pile adds is flash time, and the unit's lease has none to spare.**
  The radio's update on 13 November flashed for 14 min 44 s and the one on 14 November, 0.1.5,
  for 14 min 51 s; the Walter holds its lease on the job for 15 minutes across the flash. An
  image a little larger and meshmaster.dk fails the apply while the unit is still flashing; the
  update goes through and its result arrives late. meshmaster.dk's task 227 (a lease is never
  shortened, 45 minutes for an apply) or the Walter's pile 14, task 04 (it extends before each
  flash) ends that. Apply this pile's firmware after one of them is in place.
