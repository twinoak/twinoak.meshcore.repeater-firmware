# 01 — A person may ask again

**Depends on:** —
**Spec:** `spec/LINK-PROTOCOL.md` §8 (its last two paragraphs: the second is new, written on 21 November and copied here before this pile) and §10's row *A new attention of the same reason*; `src/common/link/attention/Attention.h` (`kAttentionLimitMs` ~L37, `raise` ~L54, the limit's two arrays ~L114) and `Attention.cpp` (`raise` ~L9–41); `src/common/power/BatteryWatch.h` (the levels ~L30, the hysteresis ~L57, the minimum time ~L60); `src/roles/repeater/link/BoardLink.cpp` (`raiseAttention` ~L309, `raiseWalterWake` ~L313); `src/roles/repeater/app/Settings.h` (~L43, ~L100); `test/test_attention/test_attention.cpp` (~L165–212), `test/test_power/test_power.cpp` (~L1245); `docs/ARCHITECTURE.md` (~L266, ~L504), `docs/link.md` (~L410, ~L1074)
**Size:** one session.
**Model:** claude-opus-5

## Goal

`AttentionMachine::raise` refuses a reason that was raised less than an hour ago, whatever the
reason. For the five reasons the firmware raises by itself that is what keeps a bad afternoon
from costing the battery an LTE attach a minute. Reason 4 is a person: somebody typed
`sensor set walter.wake 1` at the port or sent it as a mesh admin, and is waiting for the unit to
answer. Today the second such request inside the hour is taken with `ok` and raises nothing.

After this task a person's request is raised when **300 s** have passed since the last reason 4
was raised and the battery is at its normal level. While the battery is low or critical, or its
level is not known, reason 4 keeps the hour. Every other reason is what it was.

## Deliverables

1. **The limit belongs to the raise.** The machine holds a new attention to the limit that goes
   with it: the hour for reasons 1, 2, 3, 5 and 6, and for reason 4 either 300 s or the hour.
   Which of the two is decided where the battery is known (`BoardLink.cpp`, where both objects
   live) and handed to the machine; the machine stays what its header says it is: portable, no
   clock of its own, no board. The limit is counted from when the last one of that reason was
   raised, by the limit that holds when the new one is asked for: a request six minutes after
   the last is raised on a normal battery and refused on a low one, and the same request an hour
   after the last is raised on both.
2. **What *normal* is.** `BatteryWatch`'s own level, as it stands: its thresholds, its
   hysteresis, its minimum time below. Nothing of the watch changes. Say in the report what the
   watch answers before its first reading and on a board whose sensor does not answer, and make
   each of those keep the hour: *not known* is not *normal*.
3. **Still not added twice.** A reason 4 that is pending or queued is not added again, whatever
   the time; a pending attention still stays until the Walter has made a request. Only the
   refusal for age changes.
4. **The numbers that are read.** `rateLimited()` counts a refusal by either limit. Where a
   report, `STATUS` or the console prints the limit or that count, the words say what is true
   now; list the places in the report.
5. **The key's words.** `sensor set walter.wake 1` answers as it does today and
   `sensor get walter.wake` reads what it reads today. `Settings.h` says *the hourly limit* in
   two places (~L43, ~L100): the words follow the rule.
6. **Tests**, in `test/test_attention` beside the ones that hold the hour: reason 4 refused one
   millisecond before 300 s and raised at 300 s on a normal battery; held to the hour on a low
   one, a critical one and one whose level is not known; a battery that went low between two
   requests; a battery that came back to normal between two requests; each of the other five
   reasons refused one millisecond before the hour exactly as before; the millisecond clock
   wrapping between two requests of reason 4. The existing tests of the hour pass as they are,
   except where one of them used reason 4 as its example; name those.
7. **The bench**, as lines in `docs/bench.md` for a person at the port with a Walter on the
   link: two requests six minutes apart wake it twice; two requests two minutes apart wake it
   once; with `power.low_mv` set above what the pack reads, two requests six minutes apart wake
   it once. Not run by this session.
8. **The words.** `docs/ARCHITECTURE.md` and `docs/link.md` where they say one per reason per
   hour. A line in `release-notes/unreleased/` for this task.

## Acceptance criteria

- [ ] On a normal battery a second reason 4 is refused at 299 999 ms after the first and raised at 300 000 ms (the tests' names).
- [ ] On a battery that is low, critical or not known a second reason 4 is refused one millisecond before the hour and raised at the hour (the tests' names).
- [ ] Reasons 1, 2, 3, 5 and 6 are held to the hour; the tests that held them before pass unedited, and the report names every test that was edited and why.
- [ ] A reason 4 asked for while one is pending or queued is not added twice, at any age.
- [ ] No number of this rule is written twice: 300 s and the hour are each one constant.
- [ ] Nothing under upstream's tree or `spec/` was edited; `python spec/tools/specsync.py verify spec` passes.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

meshmaster.dk's own hour between two requests: its pile 4.46. The Walter: a wake a person asked
for already owes no wait there, and nothing on it counts requests. A limit a person can set. The
other five reasons. The board: no session opens a port. A release: Thomas presses the button.
