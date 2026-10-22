# 06 — MeshCore repeater-v1.18.0

**Depends on:** 05; the branch `origin/upstream/repeater-v1.18.0` (the weekly workflow's, commit `6483c18`, 19 October), with its pull request open.
**Spec:** `docs/upstream.md` (the three tiers, *The eight places a bump re-reads*, *What a bump costs today*, the pin's history); [`../sync/TEMPLATE.md`](../sync/TEMPLATE.md), the checklist this task follows as a sync task would, with the differences below; `tasks/RUNBOOK.md` (the sync note at its end); `docs/bench.md` **p0-03** (*the first Monday, and the first real bump*); `spec/SYSTEM.md` S-2, S-10, S-12, S-13; this phase's [`FOLLOWUPS.md`](FOLLOWUPS.md), its `[06]` items from 05.
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

This is the first real bump: MeshCore `repeater-v1.18.0`, 266 commits on from our pin `d9296435`
(`repeater-v1.17.1`). The workflow opened it on 19 October. Whatever its CI said, nobody has read it,
merged it or walked it. Tonight it gets all three, and the board's walks are repeated at the new pin
over everything this pile changed.

## How this differs from a sync task

- **The branch:** branch `task/p3.5-06-meshcore-1-18` from `main` (the driver's convention). Then
  `git merge origin/upstream/repeater-v1.18.0` into it, so the workflow's commit keeps its authorship
  and the pin moves exactly as the workflow moved it. Never move the pin by hand a second time.
- **The two PRs:** the PR is this task's. Once it is merged, check the workflow's PR
  (`gh pr list --head upstream/repeater-v1.18.0 --state all`). GitHub may have marked it merged by
  itself; if it is still open, close it with a comment naming this one. Never delete
  `upstream/repeater-v1.18.0`.
- **Tier three does not stop this task:** see deliverable 2 for what lands instead.

## Deliverables

1. **The reading:** the template's steps 1 to 7, every one. For each of the eight places, the report
   says *unchanged* or *changed: was X, is now Y*.
   - **Upstream's log between the pins:** after a build,
     `git -C .pio/libdeps/repeater_rak4630_single/MeshCore log --oneline d9296435..<new pin>`. Read it
     in full. The report names the commits this firmware had to answer.
   - **Commits to read closely, judged from their titles alone.** A title is a lead, not a reading.
     - `sensor-improvements` (#3402): our `sensor` variables ride on upstream's CLI.
     - `board-api-hooks` (#3131) and `missing board.loop()` (`560fb847`): our board subclass.
     - `agc.reset.interval` (#3469) and *transport code 0* (`01f9293b`): two of 05's lines.
     - the ACK forwarding delay (`6df5aaed`).
     - DISCOVER and hidden nodes (`f19733e4`).
     - `remove-room-repeater`, and `set repeat` deprecated (#3589).
     - `zero-scope` (#3568).
     - `onUnhandledResponse` and push frames (`b204f9a1`): the mesh client.
     - `MAX_SEARCH_RESULTS` (`f9e70426`).
     - the decrypt block alignment (#1809).
     - robust time (#1896): the clock keeper.
2. **Tier three:** a change to a packet layout, the login, or a CLI reply that MeshMaster or our link
   reads (`docs/upstream.md`; the agent's mesh client; the CLI pass-through of `spec/LINK-PROTOCOL.md`;
   the replies `Profile.cpp`'s reply rule and `tools/bench` depend on). **If one is found, the pin is not
   merged:**
   - the report lands on `main` by itself, through a second branch made from `main`
     (`task/p3.5-06-meshcore-1-18-report`) that carries only the report and one line in
     `docs/upstream.md`. Never a force-push;
   - `upstream/repeater-v1.18.0` stays open;
   - a follow-up asks for a person's reading, MeshMaster first.

   The task has landed.
3. **This pile's lines at the new pin:**
   - the `[06]` items 05 left;
   - `test_settings`' paging bound, the version string's length. Check it rather than assume;
   - every test written from upstream's source, re-checked against the new tag's source (template
     step 5).
4. **Verify locally** with `tools/verify.ps1` at the new pin: both roles' builds, the native tests, the
   check of `spec/`, and the release dry run. The application region is either unchanged, or gets its
   own paragraph because the Walter must be told.
5. **Walk the board at the new pin:** `bench walk 1`, `3`, `5`, `8`, `9` and `10`.
   - Every line that came back *ok* tonight must still be *ok*.
   - A *differs* that is new at 1.18 is this task's to fix if it is small. If it isn't, it is the reason
     not to merge, and the report lands as in 2.
6. **Merge** when 4 and 5 are green and 2 found nothing:
   - the pin's history row in `docs/upstream.md` says *the first real bump; read and walked by a
     session (p3.5-06)*;
   - `docs/bench.md` **p0-03** gets what this run showed. Tick what the bump showed; leave *a minute
     GitHub actually runs* to the person who saw the Actions tab.
7. **The board restored,** on this commit's repeater image.

## Acceptance criteria

- [ ] Each of the eight places read at the new tag, with the result in the report; the commits this firmware had to answer named.
- [ ] Tier three answered explicitly: *none found*, or the change and why the pin is not merged.
- [ ] `tools/verify.ps1` green at the new pin; CI green on the PR.
- [ ] Walks 1, 3, 5, 8, 9 and 10 on the board at the new pin, with result files in the report.
- [ ] Merged, with the pin's history row and p0-03 updated, or the report on `main` with the reason not to merge.
- [ ] The board restored with the tool's last line in the report.

## Out of scope

Anything 1.18 makes possible that we do not need (new boards, Wi-Fi, LR1121). A bug found on the way
gets a follow-up in the phase that owns it.
