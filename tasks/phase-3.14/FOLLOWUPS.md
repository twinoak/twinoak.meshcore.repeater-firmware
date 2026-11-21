# Phase 3.14 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas] The specification's change is not committed.** §8's second paragraph and §10's
  row are in the working tree of `twinoak.meshcore.standard-repeater-design-v2/software/spec`;
  `spec/MANIFEST.txt` here says *at commit 539467a plus uncommitted changes*.
- [ ] **[later] The Walter's copy of the specification is one paragraph behind.** Nothing in the
  Walter's firmware reads the sentence; its copy follows at its next `specsync.py copy`.
- [ ] **[later] `ok` does not say whether an attention was raised.** Upstream's CLI has two
  answers for a `set`, so `sensor set walter.wake 1` answers `ok` when the limit refused it. A
  person at a phone learns it from the unit not answering. `sensor get walter.wake` reads
  *pending*, which is true only until the Walter's first request.
- [ ] **[later] Nobody counts what a person's wakes cost the plan.** A wake was 15 to 28 kB of
  the SIM's 50 MB a month by the provider's list of 19 November; twelve an hour is about 0.3 MB
  an hour. meshmaster.dk's to show, if anybody's.
