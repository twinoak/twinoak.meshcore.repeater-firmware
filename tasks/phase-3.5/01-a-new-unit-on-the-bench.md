# 01 — A new unit on the bench

**Depends on:** phase 3.4.
**Spec:** `docs/bench.md` §0a (the rig, `bench record`, `bench flash`, `bench restore`) and §0b; `tools/bench/runs.py` (`record_board`, ~L145, and the refusal *THE RUN RECORDED NO RECOGNISABLE ROLE ON THE BOARD*, ~L188), `tools/bench/flash.py` (the touch, the bootloader's port, `found_image`), `tools/bench/walk.py` (`tick_page`, ~L1475, and the *not found in docs/bench.md* message, ~L1572), `tools/bench/fakes.py`; [`../phase-3.4/FOLLOWUPS.md`](../phase-3.4/FOLLOWUPS.md) — both `[reviewer]` items are this task; `tasks/phase-3.4/reports/02.md` (*What the walks found*: how 02 got past the first item by hand with `bench record --image`), and `out/bench/flash.log` (its first line: walk 8 failed with *the bootloader's port did not appear within 15 s of the touch* on a radio that had no application).
**Size:** one session, short.
**Model:** claude-opus-5

## Goal

Every new unit reaches this tool the way the first one did: the Walter has given it its base
(`spec/SYSTEM.md` S-5a), and it has no application, so it shows only the bootloader's port. That is
the one case the tool could not finish by itself on 22 October. The first flash worked; then
`bench restore` refused in capitals, because the run had recorded nothing it could put back. Make
the normal case for a new unit work without anyone's help, and make `--tick` say what is true.

## Deliverables

1. **`bench flash` on a board found with only its bootloader** records the image it is about to write
   as the run's `found_image`: role, version and descriptor read from the file, because the board can
   say nothing. `how: bootloader` stays in the run file, and the image is added beside it. Then
   `bench restore` puts this image back, and its last line says so:
   *the board arrived with only its bootloader; restored: the image the run began with*. Of the
   follow-up's two ways (a better message, or recording the image), this is the one that removes the
   hand from the loop. Say in the report why, and what a person would have had to type otherwise.
2. **No touch on a board with no application.** Read `flash.log`'s first line against the code: a
   board with no application has no application port to touch, and the tool should go straight to the
   bootloader's port it can already see. If 02 already made it do that, say so and name the test.
   Otherwise, fix it with a test.
3. **`--tick`:** a line that is already ticked (`- [x] `) is reported as *already ticked*. A line that
   really is missing from the page still says *not found*. Both cases get tests.
4. **Host tests first**, against `tools/bench/fakes.py` in the way 3.3 and 3.4 wrote them: a fake
   board that enumerates only as the bootloader (`239A:0029`), flashed, then restored.
5. **On the board:** the bootloader-only state cannot be recreated here, because this tool never
   erases and the base is the Walter's to write. So the walk is the board as it stands: `bench walk 8`
   once more, green, with the board restored as found. The bootloader-only path stays a host test, and
   *Not verified here* names the next new unit as the walk of it.

## Acceptance criteria

- [ ] A bootloader-only board: `bench flash`, then `bench restore`, ends with that image back and no capitals (host test).
- [ ] No touch is attempted on a board without an application port (host test), or the report shows where 02 already handles it.
- [ ] `--tick` tells *already ticked* from *not found* (host tests).
- [ ] `bench walk 8` green on the board, and the board restored with its identity (the tool's last line in the report).
- [ ] `tools/verify.ps1` green.

## Not verified here

The next radio that arrives from `rak provision` (the Walter's phase 6, task 03) is the first real
bootloader-only flash and restore.

## Out of scope

Anything below the application region, SWD and erases: the tool never does them. The scanner's
two-DFU recovery (03).
