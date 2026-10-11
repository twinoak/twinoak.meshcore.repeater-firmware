# {{NN}} — MeshCore {{TAG}}

**Depends on:** nothing. The branch already exists: `upstream/{{TAG}}`.
**Written by:** `tools/new-sync-task.ps1 -Tag {{TAG}}` on {{DATE}}.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-2, S-10, S-12;
[`docs/upstream.md`](../../docs/upstream.md); [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2.
**Size:** one session.
**Model:** claude-opus-5

## What happened

`.github/workflows/upstream.yml` saw MeshCore's `{{TAG}}` and opened **{{PR}}**
moving the pin from `{{OLD_PIN}}` to `{{NEW_PIN}}`. CI on that pull request is
**red**, which is the only case that needs a session: a green bump is merged by
a person and costs nobody anything.

The failing job:

{{LOG}}

## The task

Bring this firmware to `{{TAG}}` on the branch the workflow opened.
`git checkout upstream/{{TAG}}` (it is on `origin`; `git pull` it) and work
there — do **not** branch from `main` and do **not** move the pin a second
time. The workflow's commit is the first commit of this task; yours come after
it, and the pull request is already open, so CI runs on every push.

1. **Read what upstream changed.** The pull request body lists MeshCore's
   commits between the two pins. After a build, the same thing with the diff
   attached is
   `git -C .pio/libdeps/repeater_rak4630_single/MeshCore log --oneline {{OLD_PIN}}..{{NEW_PIN}}` —
   and that checkout, not a transcription of it and not this file, is what a
   claim about MeshCore is checked against (`tasks/README.md`).
2. **The two literals first.** `test/test_version/test_version.cpp`
   (`Version.ThisBuildSaysThePinOfPlatformioIni`) asserts the whole version
   string with the old tag and the old pin's first eight characters in it, and
   `tools/release/test_release.py`'s synthetic `buildinfo.json` names the old
   tag. Both say `{{TAG}}` / `{{NEW_PIN}}` after this bump. They are why a bump
   is red before anything of upstream's has even been looked at
   (`docs/upstream.md`, *What a bump costs today*) — so do them first and get a
   clean signal for the rest.
3. **Re-read the seven places a bump re-reads.** The table in
   `docs/upstream.md` is the list; each row says what it depends on upstream
   for. In short:
   1. the flag blocks copied into `platformio.ini` — `diff` our `[nrf52_base]`
      and `[env:repeater_rak4630_single]` against upstream's `[arduino_base]`,
      `[nrf52_base]`, `[sensor_base]` (its root `platformio.ini`) and
      `[rak4631]`, `[env:RAK_4631_repeater]`
      (`variants/rak4631/platformio.ini`) at the new tag. Bring across what
      moved, and put a line in the report for anything deliberately not
      brought across;
   2. `[libs]` — re-resolve the exact versions against upstream's new ranges
      (`library.json` and its `platformio.ini`), ours still first in
      `lib_deps`;
   3. `boards/rak4631.json` — re-copy upstream's and `diff` it; the one file of
      upstream's in this repository (`boards/README.md`);
   4. `tools/pio/*` — `meshcore_private_libs.py` wants `lib/ed25519`,
      `meshcore_uf2.py` wants `bin/uf2conv/uf2conv.py`. Both raise if the pin
      moved them, which is a build that fails and not a question;
   5. every place that cites a line of upstream's at the old pin:
      `grep -rn "{{OLD_PIN_SHORT}}" --include=*.cpp --include=*.h --include=*.py --include=*.md --include=*.ini .`
      finds the ones that say so out loud, and a comment in `src/` that names a
      file and a function of MeshCore's is read at the new tag and re-cited;
   6. `build_as_lib.py`'s defines — `MC_VARIANT`, `BUILD_EXAMPLE` and (from
      phase 1) `EXCLUDE_FROM_EXAMPLE`: read upstream's script at the new tag
      and check it still selects the sources we think it does;
   7. the application region in the linker script — the release tooling fails
      if an image falls outside it, so a move is caught; but the manifest's
      `app_region` then **changes**, and the Walter writes images at that
      address. If it moved, the report says so loudly and in its own paragraph,
      because the other box has to be told.
4. **Bring the code to the new API.** A rename upstream is a rename here; a
   changed packet layout is a changed reader here. What is **not** allowed is
   to work around a compile error by loosening a rail, by editing a file of
   upstream's, or by copying one of its sources into `src/`
   (`spec/SYSTEM.md` S-2: our application subclasses and composes).
5. **Re-run every test that was written from upstream's source against the new
   tag's source.** A test that encodes or decodes a MeshCore frame, or that
   pins the shape of one of its replies, is only as right as the source it was
   read from: check each against
   `.pio/libdeps/repeater_rak4630_single/MeshCore` at `{{NEW_PIN}}`, not
   against the old reading of it. The `spec/` fixtures are **not** ours to
   change (`spec/README.md`): a fixture that disagrees with the new upstream is
   tier three of `docs/upstream.md` and a finding for the report, not an edit.
6. **Verify locally, then push.** `tools/verify.ps1` — every board
   environment's build, the native tests, `spec/` against its manifest, the dry
   run of the release — green on this machine before anything is pushed
   (`spec/SYSTEM.md` S-10). Then push to `upstream/{{TAG}}`; the pull request
   is open, so CI runs on the push.
7. **Add the row to the pin's history** in `docs/upstream.md` if the workflow's
   own commit did not (it does, for a bump it opened), and say in that row's
   *How* column that it took a session.

## The rails that must survive this bump

Every one of these is in the repository README, and a bump is exactly when one
gets quietly lost:

- **It repeats, whatever else happens.** Nothing brought across may stall the
  mesh loop.
- **Not one file of upstream's tree is edited** — not in
  `.pio/libdeps/*/MeshCore`, not by a script, not "just to see". `git status`
  in that checkout is empty after a build and stays that way.
- **Nothing the mesh says is logged as text.** `MESH_PACKET_LOGGING` and
  `MESH_DEBUG` stay off whatever upstream's examples do, and whatever a new
  define of theirs defaults to.
- **No default password is ever on the air** (`spec/SYSTEM.md` S-13), and a
  password is in RAM for the length of a send and nowhere else.
- **One packet per send** for everything the mesh client originates, and path
  hashes this firmware chooses are two bytes or more.
- **Never a chip erase, and never outside the application region.**
- **`spec/` is a copy**: `python spec/tools/specsync.py verify spec` passes,
  and no file under it is edited here.

## Acceptance criteria

- [ ] `tools/verify.ps1` green at `{{NEW_PIN}}` on the machine the session ran on, and then in CI on the pull request.
- [ ] Each of the seven places of `docs/upstream.md` read at the new tag, and the report says for each: *unchanged*, or *changed: was X, is now Y*.
- [ ] Every test written from upstream's source re-checked against the new tag's source, and the rails above each named in the report with the test or the grep that holds it.
- [ ] The report says what upstream changed **that mattered** — not "the pin moved", but which of their commits this session had to answer.
- [ ] If the application region moved: its own paragraph in the report, and the new `app_region` named, because the Walter has to be told (`spec/SYSTEM.md` S-5).

## Out of scope

Anything that is not this bump. A bug found on the way gets a line in the
follow-ups of the phase that owns it (`tasks/phase-*/FOLLOWUPS.md`), tagged for
the task that owns it. A change upstream makes to the **protocol** is tier three
of `docs/upstream.md` — MeshMaster first, and a reading task before it is a code
change — and a bump that runs into one stops and says so
(`tasks/RUNBOOK.md` §5).
