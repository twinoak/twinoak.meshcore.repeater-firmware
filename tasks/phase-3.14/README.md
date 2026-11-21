# Phase 3.14 — a person may ask for the Walter again

Written on 21 November.

**What happened.** Thomas asked the unit at Bellevue to wake twice that evening. Both the radio
and meshmaster.dk allow one request an hour: `spec/LINK-PROTOCOL.md` §8 said *at most one new
attention per reason per hour*, and the server refuses a second request inside the hour because
the radio would raise nothing for it.

**What Thomas decided.** The hour is artificial for a person: if the battery can bear it, he
should be able to ask every five minutes.

**What that became.** §8 has a second paragraph: reason 4 is raised again when 300 s have passed
since the last one, while the battery is at its normal level; low, critical or not known keeps
the hour. The other five reasons are untouched.

**Chosen by me, his to change before the run:**

- **300 s, and not no limit at all.** meshmaster.dk sends the wake line up to three times when
  its reply is lost, two minutes and then five minutes apart. A line that did arrive must not
  wake the Walter a second time two minutes later.
- **The hour stays on a battery that is low, critical or not known.** His condition was *if the
  repeater can honour the request with regard to the battery*; the firmware's own word for that
  is the level `BatteryWatch` already keeps.

**The specification** was changed where it lives, in the working tree of
`twinoak.meshcore.standard-repeater-design-v2/software/spec` (§8's new paragraph and one row of
§10), and copied here with `specsync.py copy` before this pile: `spec/LINK-PROTOCOL.md` and
`spec/MANIFEST.txt`. No byte on the wire changes and the link's version does not move.

**Not on the board.** No session opens a port. No release: Thomas presses the button.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-person-may-ask-again](01-a-person-may-ask-again.md) | — | reason 4 raised again 300 s after the last on a normal battery and an hour after it otherwise; the other reasons as they were; tests, the bench's lines, the words |

```powershell
.\tasks\run-all.ps1 -Phase '3.14'
```

## Models

- **Opus**: 01.

## Afterwards (Thomas)

- **Commit the specification** in the hardware repository: §8 and §10 of
  `software/spec/LINK-PROTOCOL.md` are uncommitted there.
- **Release** with the button, merge its notes pull request, then offer and apply the radio's
  firmware to the unit from meshmaster.dk.
- **meshmaster.dk's half** is its pile 4.46. Either may go first; until both are in service the
  stricter of the two rules is what a person meets.
- **The bench, when there is time:** task 01's lines in `docs/bench.md`.

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
