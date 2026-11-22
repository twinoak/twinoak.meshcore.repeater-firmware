# Phase 3.15 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[Thomas] Nobody has seen task 01 on a chip.** The cause is read from numbers and from
  RadioLib's source. The answer is a session on the unit with one step,
  `20 hist 869.418 869.818 25`, after the release: sixteen lines that each add up to 2 048, or
  an `error` line that names a sum.
- [ ] **[later, Thomas to decide] RadioLib's example asks the status the same way.** If the
  session shows the register does keep *completed* across a start, that is a report for
  upstream. His to file.
- [ ] **[later] `hist` sends as fast as it measures.** At the least `samples` (256) a scan is
  2 ms and the port is full for the whole pass. The Walter's side has to keep up with a full
  port (its pile 18, task 03); nothing here slows a pass down.
