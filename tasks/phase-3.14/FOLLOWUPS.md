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

## The review of 01 (22 November)

Merged 21 November 21:40 (#58, main b13bdeb), 47 minutes. Read: the whole diff under `src/`
(`Attention.h/.cpp`, `BatteryWatch.h`, `BoardPower.h/.cpp`, `BoardLink.h/.cpp`, the comments in
`Settings.h`, `Interfaces.h`, `RoleHooks.h`), the report, the names of the new tests. Not read
line by line: the tests' bodies, `tools/linkloop/`, the docs. Nothing found that asks for a
task.

- [ ] **[none] What was built is the rule and no more.** `raise()` takes the limit as a fourth
  argument that defaults to the hour, so the other five reasons and every existing call are
  what they were; `personAskedLimitMs(battery_normal)` is the one place 300 s and the hour
  meet; `raiseWalterWake()` asks `power::batteryKnownNormal()`. Eight of eight criteria.
- [ ] **[none] *Not known* is the reading's validity bit, not the level.** The watch answers
  `normal` before its first reading and holds its level through a sensor that stops, so the
  session made *known* a fact of its own (`levelKnownNormal(level, reading_valid)`), which is
  what the specification's paragraph asks for. A sensor in its five-second back-off also keeps
  the hour for those seconds; that is the cautious way round.
- [ ] **[none] The first minute of a boot reads a low pack as normal** (the watch's own minute
  before it believes a level). It cannot matter here: a restart forgets when reason 4 was last
  raised, so the first request is raised whatever the limit, and 300 s is longer than the
  minute.
- [ ] **[Thomas] Not on a unit yet.** `VERSION` is 0.1.7 with this task's one note waiting; the
  unit at Bellevue runs 0.1.6 and still raises one attention an hour. The three bench lines of
  `docs/bench.md` §4 need a Walter or `linkcli.py` on the link and two six-minute waits.

## The scanner's histogram, as session 1 read it (21 November)

From the lines of the first NoiseScope session on a unit (meshmaster.dk `/noisescope/1`, scanner
0.1.6+fcf52d5, `SX1261 V2D 2D02`, over the Walter's link). No code was read for it.

- [ ] **[pile 3.15, task 01] `hist` over more than one frequency does not add up.** The step
  was `hist 869.418 869.818 25`: sixteen frequencies, 2 048 samples each. The seven lines that
  arrived are 22 ms apart (`ms` 272006 to 272141), and *done* came at 272356: a third of a
  second for the whole sweep. Each says `"n":2048`, `"b0":-11`, `"bw_db":4`. The bins of the
  first, 869.418 MHz, are 373 + 1 525 + 150 = 2 048, around -131 dBm. The others hold 15, 15, 8,
  **4 140** (4 096 of them in the -59 dBm bin, at 869.518 MHz), 32 and 1 807 (1 792 at
  -43 dBm, at 869.568 MHz). A count above the samples asked for, and counts far below it, are
  not a histogram of anything. The first frequency of a sweep looks right; what follows it does
  not.
- [ ] **[later] Sixteen lines in a third of a second is more than the Walter's link kept.** Of
  `seq` 245 to 260 the server holds 245 to 251 (the Walter's pile 18, task 03).
  A scope sends a frame a second for that port; `hist` sends as fast as it measures.
