# 04 — The garden is the bench

**Depends on:** 03 (the frame's new members and the four words).
**Spec:** `tasks/phase-3.16/README.md` (*The garden instead of the bench*, with the four lists as lines); `tools/noisescope/hunt.py`, `echoes.py`, `test_hunt.py`, `test_echoes.py`; `tasks/phase-3.16/reports/03.md` (a frame's bytes); `docs/noisescope.md` §5 and §7.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The echoes are measured on the unit at Bellevue, in NoiseScope sessions started from
meshmaster.dk (the README says why). A session is not a serial port: its program is at most
six typed steps, it comes back as a file of lines an hour later, and the signal that lights
the echoes is a neighbour's traffic, there in some frames and not in others. `hunt.py` and
`echoes.py` were built for a board on USB with a steady carrier.

This task makes the two tools work the garden's way: a list becomes session programs that
are known to be taken before anybody types them, and a session's download becomes one table
across its steps with the verdicts the hunt is after. **Tools and words only: nothing under
`src/` changes.**

## What a session takes (read on 24 November; these are other repositories' numbers)

- A program is 1 to 6 steps. A step lasts 5 to 300 seconds; all of them 600 at most.
- A step's command is 1 to 40 characters of `a-z`, `0-9`, space and the full stop, and its
  first word is one of `stat sweep scope hist dwell cad`.
- The program as it goes out is at most 384 bytes of
  `{"slot":0,"steps":[{"command":"…","seconds":N},…]}`.
  (meshmaster.dk `src/MeshMaster.Contracts/NoiseScope.cs`, `NoiseScopeProgram`; the Walter's
  `components/core/include/core/update/scanner_session.h`: `kStepsMax` 6, `kCommandMax` 40.)
- The Walter keeps 65 536 bytes of lines a step; a step that fills it ends `full`. The
  server keeps 512 kB and 4 000 lines a session.
- On meshmaster.dk's form a step is typed as one line, the seconds and then the command:
  `60 scope 868.618 870.618 250 0 1000`.
- A session's lines come back from its page's *Download the lines*: every line as the scanner
  sent it, one JSON object a line, named `noisescope-<repeater>-<yyyyMMdd-HHmmss>.ndjson`.

Those repositories are not this task's to read or change. Hold the numbers as named
constants in one place with this paragraph's sources beside them.

## Deliverables

1. **`hunt.py --sessions <list>`: a list as session programs, and no board.**
   - The list is `hunt.py`'s own format (`name = <the line>`, `#` a comment) with one
     addition: a line `--- <name>` begins a new session, and `seconds = N` sets the seconds
     of the steps after it (85 when not said).
   - Lines are **not** rewritten to interval 0 and `last` here: a garden step is typed with
     its own interval and aggregate, because the signal is not steady.
   - Every rule of *What a session takes* is checked, and a list that breaks one is refused
     with the session, the step and the number that broke: a character, 40 characters, six
     steps, the seconds, 600 seconds, 384 bytes worked out from the same JSON, and a step
     whose frames would not fit the Walter's 65 536 bytes (frames from the seconds and the
     interval, a frame's bytes from the plan's points and 03's report; the arithmetic in one
     function with a test against real lines of `tools/noisescope/captures/`).
   - It prints, for each session: the lines to paste into the form, the bytes as it goes
     out, the seconds, the bytes each step is expected to bring, and how long the repeater
     is off the mesh (the README's figure for the two flashes plus the steps). `--json` for
     the same.
   - Exit 0 when every session of the list would be taken, 2 when one would not. No port is
     opened and `tools/bench` is not asked for anything.
2. **The four lists**, as files: `tools/noisescope/lists/garden-a.txt` to `garden-d.txt`,
   the lines of the README's *The garden instead of the bench* with a comment over each
   saying what it asks. `garden-a` uses nothing 02 or 03 added (it is run on the scanner of
   0.1.7 before this task exists; its file is for the record and for the tool's test). If
   03's report gives a frame more bytes than the README counted with, shorten the seconds,
   not the lines, and say so.
3. **`echoes.py --steps <download.ndjson>`: one table across a session's scopes**, in the
   order they were asked, a row a scope:
   - the span, the bandwidth, the points and segments, the width and settle, the regulator
     and the gain (from the frames and the scope's `ack`);
   - the frames, and the frames that hold the signal (`--carrier`, `--lit`);
   - the echoes in those frames: how many places, and each place's first point in MHz with
     its distance from its segment's first point and from the carrier, and its strongest
     reading against the carrier's in the same frame;
   - the time a point cost: `us` over the points, shortest and longest over the scope, with
     1.6 / RBW and 0.94 / RBW beside it (absent for a capture made before 03);
   - `dev` OR-ed over the scope, with its bits named when it is not `0000` (absent before
     03, and for one segment).
4. **Pairs, and what they say.** Under the table, for every two scopes of the capture that
   differ in **exactly one** thing:
   - *the first point* (same step, bandwidth, width, settle, regulator, gain): for each
     echo of either, whether the other has one at the same frequency, at the same distance
     from its segment's first point, at both, or at neither, each within one step. Then one
     line: **stays with the frequency**, **moves with the segments**, or **cannot say**,
     with the counts. *Cannot say* is the answer when the two first points differ by a
     multiple of the interval the echoes keep (both readings predict the same places) and
     when fewer than three echoes were found in either: say which.
   - *the regulator*, *the gain*, *the width*, *the settle*: the echoes of one that the
     other does not have, and for those both have, the difference in dB against the
     carrier. One line each. No verdict beyond the counts.
   The rules are these and nothing learned from the data: the thresholds are `--lit` and
   `--over`, as today.
5. **A signal that comes and goes.** `echoes.py`'s floor is read across the frequency axis
   because the bench's carrier is in every frame. A garden capture has quiet frames: for
   `--steps`, a place counts as an echo when it stands over the same place's median in the
   scope's own quiet frames by `--over`, when the scope has five quiet frames or more, and
   as today when it has not. The table says which rule each row used. The frames of
   `/noisescope/3` read the same places 4 to 8 dB high even when quiet: print that
   difference per place too (quiet median against the segment's floor), it is part of what
   is hunted.
6. **Tests** (`test_hunt.py`, `test_echoes.py`; standard library; in the release step as the
   others are): each rule of deliverable 1 broken once and named in the refusal; the four
   lists taken; a planted capture whose echoes follow the segments and one whose echoes
   follow the carrier, each read rightly by the pair's line, and one where the shift is a
   multiple of the interval read as *cannot say*; a capture without 03's members; the
   quiet-frame rule with four quiet frames and with five.
7. **Captures kept:** `tools/noisescope/captures/garden/README.md` says where a download
   goes (its own name, unedited) and the one command over it. If
   `tools/noisescope/captures/` holds a download of session 3 or later when this runs, run
   `--steps` over each and put the tables in the report. If it holds none, say so.
8. **The words.** `docs/noisescope.md` §7: the garden's way in one short section (a list,
   `--sessions`, the form, the download, `--steps`), and what a session cannot show (a level
   that is known, the dummy load, an attenuator). `hunt.py`'s and `echoes.py`'s own headers.
   No release note: nothing a unit runs changes.

## Acceptance criteria

- [ ] `python tools/noisescope/hunt.py --sessions tools/noisescope/lists/garden-b.txt` prints a program meshmaster.dk's rules take, and each rule broken is refused by name (the tests' names; the four programs printed in the report).
- [ ] `--sessions` opens no port and imports nothing that does (the test's name).
- [ ] `echoes.py --steps` over a planted capture gives the table of deliverable 3 and the pair lines of deliverable 4, with *moves with the segments*, *stays with the frequency* and *cannot say* each produced by a test.
- [ ] A capture made before 03 is read, with the two columns empty.
- [ ] The quiet-frame rule is used when there are five quiet frames and not when there are four, and the row says which.
- [ ] `echoes.py` without `--steps` and `hunt.py` without `--sessions` do what they do on `main` (their tests, unedited).
- [ ] `tools\verify.ps1` green. CI green.

## What only the unit can show

The verdicts themselves. Thomas starts the sessions of `garden-b`, `-c` and `-d` from
meshmaster.dk once a release carries 03, saves each download into
`tools/noisescope/captures/garden/`, and I run `--steps` over them.

## Out of scope

Anything under `src/`. The default of `scope`. meshmaster.dk's page and its proposal. A
session of more than six steps or longer lines (other repositories' contract). The bench
lists and what `hunt.py` does with a board.
